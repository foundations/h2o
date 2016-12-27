/*
 * Copyright (c) 2016 DeNA Co., Ltd., Ichito Nagata
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include <mruby.h>
#include <mruby/array.h>
#include <mruby/error.h>
#include <mruby/hash.h>
#include <mruby/string.h>
#include <mruby/variable.h>
#include "h2o/mruby_.h"
#include "h2o/redis.h"

struct st_h2o_mruby_redis_conn_t {
    h2o_redis_conn_t super;
    h2o_mruby_context_t *ctx;
    struct {
        mrb_value redis;
    } refs;
};

struct st_h2o_mruby_redis_command_context_t {
    h2o_mruby_generator_t *generator;
    mrb_value receiver;
    struct {
        mrb_value command;
    } refs;
};

static void *reader_create_string_object(const redisReadTask *task, char *str, size_t len);
static void *reader_create_array_object(const redisReadTask *task, int elements);
static void *reader_create_integer_object(const redisReadTask *task, long long value);
static void *reader_create_nil_object(const redisReadTask *task);
static void reader_free_object(void *ptr);

redisReplyObjectFunctions reader_object_functions = {
    reader_create_string_object,
    reader_create_array_object,
    reader_create_integer_object,
    reader_create_nil_object,
    reader_free_object
};

static void on_gc_dispose_redis(mrb_state *mrb, void *_conn)
{
    struct st_h2o_mruby_redis_conn_t *conn = _conn;
    if (conn == NULL) return;

    h2o_redis_free(&conn->super);
    conn->refs.redis = mrb_nil_value();
}

static void on_gc_dispose_command(mrb_state *mrb, void *_ctx)
{
    struct st_h2o_mruby_redis_command_context_t *ctx = _ctx;
    if (ctx == NULL) return;

    ctx->refs.command = mrb_nil_value();
}

const static struct mrb_data_type redis_type = {"redis", on_gc_dispose_redis};
const static struct mrb_data_type command_type = {"redis_command", on_gc_dispose_command};

static mrb_value create_downstream_closed_exception(mrb_state *mrb)
{
    return mrb_exc_new_str_lit(mrb, E_RUNTIME_ERROR, "downstream HTTP closed");
}

static mrb_value detach_receiver(struct st_h2o_mruby_redis_command_context_t *ctx)
{
    mrb_value ret = ctx->receiver;
    assert(!mrb_nil_p(ret));
    ctx->receiver = mrb_nil_value();
    return ret;
}

static void on_command_dispose(void *_ctx)
{
    struct st_h2o_mruby_redis_command_context_t *ctx = _ctx;

    /* TODO: is this necessary? */
    if (!mrb_nil_p(ctx->refs.command))
        DATA_PTR(ctx->refs.command) = NULL;

    /* notify the app, if it is waiting to hear from us */
    if (!mrb_nil_p(ctx->receiver)) {
        mrb_state *mrb = ctx->generator->ctx->shared->mrb;
        int gc_arena = mrb_gc_arena_save(mrb);
        h2o_mruby_run_fiber(ctx->generator, detach_receiver(ctx), create_downstream_closed_exception(mrb), NULL);
        mrb_gc_arena_restore(mrb, gc_arena);
    }
}

static redisReader *create_reader(h2o_redis_conn_t *_conn)
{
    struct st_h2o_mruby_redis_conn_t *conn = (void *)_conn;
    redisReader *reader = redisReaderCreate();
    reader->fn = &reader_object_functions;
    reader->privdata = conn;
    return reader;
}

static mrb_value setup_method(mrb_state *mrb, mrb_value self)
{
    assert(h2o_mruby_initializing_context != NULL);

    struct st_h2o_mruby_redis_conn_t *conn = (struct st_h2o_mruby_redis_conn_t *)h2o_redis_create_connection(h2o_mruby_initializing_context->ctx->loop, sizeof(*conn));
    conn->ctx = h2o_mruby_initializing_context;
    conn->super.create_reader = create_reader;

    DATA_TYPE(self) = &redis_type;
    DATA_PTR(self) = conn;

    return self;
}

static mrb_value connect_method(mrb_state *mrb, mrb_value self)
{
    struct st_h2o_mruby_redis_conn_t *conn = DATA_PTR(self);
    if (conn->super.state != H2O_REDIS_CONNECTION_STATE_CLOSED)
        return self;

    mrb_value config = mrb_iv_get(mrb, self, mrb_intern_lit(mrb, "@config"));
    mrb_value _host = mrb_hash_get(mrb, config, mrb_symbol_value(mrb_intern_lit(mrb, "host")));
    mrb_value _port = mrb_hash_get(mrb, config, mrb_symbol_value(mrb_intern_lit(mrb, "port")));
    char *host = mrb_str_to_cstr(mrb, _host);
    uint16_t port = mrb_fixnum(_port);

    h2o_redis_connect(&conn->super, host, port);

    return self;
}

static mrb_value disconnect_method(mrb_state *mrb, mrb_value self)
{
    struct st_h2o_mruby_redis_conn_t *conn = DATA_PTR(self);
    h2o_redis_disconnect(&conn->super);
    return self;
}

static void on_redis_command(void *_reply, void *_ctx)
{
    struct st_h2o_mruby_redis_command_context_t *ctx = _ctx;
    mrb_state *mrb = ctx->generator->ctx->shared->mrb;
    mrb_value reply;

    if (_reply == NULL) {
        /* TODO: throw specific io error? */
        reply = mrb_exc_new_str_lit(mrb, E_RUNTIME_ERROR, "redis command failed due to some connection problems");
    } else {
        reply = *(mrb_value *)_reply;
    }

    if (mrb_nil_p(ctx->receiver)) {
        mrb_funcall(mrb, ctx->refs.command, "_set_reply", 1, reply);
        if (mrb->exc != NULL) {
            fprintf(stderr, "_set_reply failed\n");
            abort();
        }
    } else {
        int gc_arena = mrb_gc_arena_save(mrb);
        h2o_mruby_run_fiber(ctx->generator, detach_receiver(ctx), reply, NULL);
        mrb_gc_arena_restore(mrb, gc_arena);
    }
}

static mrb_value call_method(mrb_state *mrb, mrb_value self)
{
    struct st_h2o_mruby_redis_conn_t *conn = DATA_PTR(self);
    h2o_mruby_generator_t *generator;
    mrb_int i;

    /* precond check */
    if ((generator = h2o_mruby_current_generator) == NULL || generator->req == NULL)
        mrb_exc_raise(mrb, create_downstream_closed_exception(mrb));


    /* allocate context and initialize */
    struct st_h2o_mruby_redis_command_context_t *command_ctx = h2o_mem_alloc_shared(&generator->req->pool, sizeof(*command_ctx), on_command_dispose);
    memset(command_ctx, 0, sizeof(*command_ctx));
    command_ctx->generator = generator;
    command_ctx->receiver = mrb_nil_value();
    command_ctx->refs.command = h2o_mruby_create_data_instance(mrb, mrb_ary_entry(generator->ctx->shared->constants, H2O_MRUBY_REDIS_COMMAND_CLASS), command_ctx, &command_type);

    /* retrieve argument array */
    mrb_value *command_args;
    mrb_int command_len;
    mrb_get_args(mrb, "a", &command_args, &command_len);

    const char **argv = h2o_mem_alloc(command_len * sizeof(char *));
    size_t *argvlen = h2o_mem_alloc(command_len * sizeof(size_t));
    for (i = 0; i != command_len; ++i) {
        int gc_arena = mrb_gc_arena_save(mrb);
        mrb_value s = mrb_obj_as_string(mrb, command_args[i]);
        argv[i] = mrb_str_to_cstr(mrb, s);
        argvlen[i] = mrb_string_value_len(mrb, s);
        mrb_gc_arena_restore(mrb, gc_arena);
    }

    /* send command to redis */
    h2o_redis_command_argv(&conn->super, on_redis_command, command_ctx, (int)command_len, argv, argvlen);

    free(argv);
    free(argvlen);

    return command_ctx->refs.command;
}

void h2o_mruby_redis_init_context(h2o_mruby_shared_context_t *ctx)
{
    mrb_state *mrb = ctx->mrb;

    struct RClass *module = mrb_define_module(mrb, "H2O");

    h2o_mruby_define_callback(mrb, "_h2o__redis_join_reply", H2O_MRUBY_CALLBACK_ID_REDIS_JOIN_REPLY);

    struct RClass *redis_klass = mrb_class_get_under(mrb, module, "Redis");
    mrb_define_method(mrb, redis_klass, "__setup", setup_method, MRB_ARGS_NONE());
    mrb_define_method(mrb, redis_klass, "connect", connect_method, MRB_ARGS_NONE());
    mrb_define_method(mrb, redis_klass, "disconnect", disconnect_method, MRB_ARGS_NONE());
    mrb_define_method(mrb, redis_klass, "__call", call_method, MRB_ARGS_ARG(1, 0));

    struct RClass *redis_command_klass = mrb_class_get_under(mrb, redis_klass, "Command");
    mrb_ary_set(mrb, ctx->constants, H2O_MRUBY_REDIS_COMMAND_CLASS, mrb_obj_value(redis_command_klass));
}

mrb_value h2o_mruby_redis_join_reply_callback(h2o_mruby_generator_t *generator, mrb_value receiver, mrb_value args,
                                                int *next_action)
{
    mrb_state *mrb = generator->ctx->shared->mrb;
    struct st_h2o_mruby_redis_command_context_t *ctx;

    if (generator->req == NULL)
        return create_downstream_closed_exception(mrb);

    if ((ctx = mrb_data_check_get_ptr(mrb, mrb_ary_entry(args, 0), &command_type)) == NULL)
        return mrb_exc_new_str_lit(mrb, E_ARGUMENT_ERROR, "Redis::Command#join wrong self");

    ctx->receiver = receiver;
    *next_action = H2O_MRUBY_CALLBACK_NEXT_ACTION_ASYNC;
    return mrb_nil_value();
}


/* reader object functions */

static void *try_parentize(mrb_state *mrb, const redisReadTask *task, mrb_value *v) {
    if (task && task->parent != NULL) {
        mrb_value *parent = (mrb_value *)task->parent->obj;
        assert(mrb_array_p(*parent));
        mrb_ary_set(mrb, *parent, task->idx, *v);
    }
    return (void *)v;
}

static void *reader_create_string_object(const redisReadTask *task, char *str, size_t len) {
    struct st_h2o_mruby_redis_conn_t *ctx = task->privdata;
    mrb_state *mrb = ctx->ctx->shared->mrb;
    mrb_value v = mrb_str_new(mrb, str, len);

    /* TODO: how to handle string encoding? */

    if (task->type == REDIS_REPLY_ERROR) {
        v = mrb_exc_new_str(mrb, E_RUNTIME_ERROR, v);
    }

    return try_parentize(mrb, task, &v);
}

static void *reader_create_array_object(const redisReadTask *task, int elements) {
    struct st_h2o_mruby_redis_conn_t *conn = task->privdata;
    mrb_state *mrb = conn->ctx->shared->mrb;
    mrb_value v = mrb_ary_new_capa(mrb, elements);
    return try_parentize(mrb, task, &v);
}

static void *reader_create_integer_object(const redisReadTask *task, long long value) {
    struct st_h2o_mruby_redis_conn_t *conn = task->privdata;
    mrb_state *mrb = conn->ctx->shared->mrb;
    mrb_value v = mrb_fixnum_value(value);
    return try_parentize(mrb, task, &v);
}

static void *reader_create_nil_object(const redisReadTask *task) {
    struct st_h2o_mruby_redis_conn_t *conn = task->privdata;
    mrb_state *mrb = conn->ctx->shared->mrb;
    mrb_value v = mrb_nil_value();
    return try_parentize(mrb, task, &v);
}

static void reader_free_object(void *ptr) {
    /* do nothing */
}