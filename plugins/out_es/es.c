/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */

/*  Fluent Bit
 *  ==========
 *  Copyright (C) 2019-2021 The Fluent Bit Authors
 *  Copyright (C) 2015-2018 Treasure Data Inc.
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 */

#include <fluent-bit/flb_output_plugin.h>
#include <fluent-bit/flb_utils.h>
#include <fluent-bit/flb_network.h>
#include <fluent-bit/flb_http_client.h>
#include <fluent-bit/flb_pack.h>
#include <fluent-bit/flb_time.h>
#include <fluent-bit/flb_signv4.h>
#include <fluent-bit/flb_aws_credentials.h>
#include <fluent-bit/flb_record_accessor.h>
#include <msgpack.h>

#include <time.h>

#include "es.h"
#include "es_conf.h"
#include "es_bulk.h"
#include "murmur3.h"

struct flb_output_plugin out_es_plugin;

static int es_pack_array_content(msgpack_packer *tmp_pck,
                                 msgpack_object array,
                                 struct flb_elasticsearch *ctx);

#ifdef FLB_HAVE_AWS
static flb_sds_t add_aws_auth(struct flb_http_client *c,
                              struct flb_elasticsearch *ctx)
{
    flb_sds_t signature = NULL;
    int ret;

    flb_plg_debug(ctx->ins, "Signing request with AWS Sigv4");

    /* Amazon ES Sigv4 does not allow the host header to include the port */
    ret = flb_http_strip_port_from_host(c);
    if (ret < 0) {
        flb_plg_error(ctx->ins, "could not strip port from host for sigv4");
        return NULL;
    }

    /* AWS Fluent Bit user agent */
    flb_http_add_header(c, "User-Agent", 10, "aws-fluent-bit-plugin", 21);

    signature = flb_signv4_do(c, FLB_TRUE, FLB_TRUE, time(NULL),
                              ctx->aws_region, "es",
                              0,
                              ctx->aws_provider);
    if (!signature) {
        flb_plg_error(ctx->ins, "could not sign request with sigv4");
        return NULL;
    }
    return signature;
}
#endif /* FLB_HAVE_AWS */

static int es_pack_map_content(msgpack_packer *tmp_pck,
                               msgpack_object map,
                               struct flb_elasticsearch *ctx)
{
    int i;
    char *ptr_key = NULL;
    char buf_key[256];
    msgpack_object *k;
    msgpack_object *v;

    for (i = 0; i < map.via.map.size; i++) {
        k = &map.via.map.ptr[i].key;
        v = &map.via.map.ptr[i].val;
        ptr_key = NULL;

        /* Store key */
        const char *key_ptr = NULL;
        size_t key_size = 0;

        if (k->type == MSGPACK_OBJECT_BIN) {
            key_ptr  = k->via.bin.ptr;
            key_size = k->via.bin.size;
        }
        else if (k->type == MSGPACK_OBJECT_STR) {
            key_ptr  = k->via.str.ptr;
            key_size = k->via.str.size;
        }

        if (key_size < (sizeof(buf_key) - 1)) {
            memcpy(buf_key, key_ptr, key_size);
            buf_key[key_size] = '\0';
            ptr_key = buf_key;
        }
        else {
            /* Long map keys have a performance penalty */
            ptr_key = flb_malloc(key_size + 1);
            if (!ptr_key) {
                flb_errno();
                return -1;
            }

            memcpy(ptr_key, key_ptr, key_size);
            ptr_key[key_size] = '\0';
        }

        /*
         * Sanitize key name, Elastic Search 2.x don't allow dots
         * in field names:
         *
         *   https://goo.gl/R5NMTr
         */
        if (ctx->replace_dots == FLB_TRUE) {
            char *p   = ptr_key;
            char *end = ptr_key + key_size;
            while (p != end) {
                if (*p == '.') *p = '_';
                p++;
            }
        }

        /* Append the key */
        msgpack_pack_str(tmp_pck, key_size);
        msgpack_pack_str_body(tmp_pck, ptr_key, key_size);

        /* Release temporary key if was allocated */
        if (ptr_key && ptr_key != buf_key) {
            flb_free(ptr_key);
        }
        ptr_key = NULL;

        /*
         * The value can be any data type, if it's a map we need to
         * sanitize to avoid dots.
         */
        if (v->type == MSGPACK_OBJECT_MAP) {
            msgpack_pack_map(tmp_pck, v->via.map.size);
            es_pack_map_content(tmp_pck, *v, ctx);
        }
        /*
         * The value can be any data type, if it's an array we need to
         * pass it to es_pack_array_content.
         */
        else if (v->type == MSGPACK_OBJECT_ARRAY) {
          msgpack_pack_array(tmp_pck, v->via.array.size);
          es_pack_array_content(tmp_pck, *v, ctx);
        }
        else {
            msgpack_pack_object(tmp_pck, *v);
        }
    }
    return 0;
}

/*
  * Iterate through the array and sanitize elements.
  * Mutual recursion with es_pack_map_content.
  */
static int es_pack_array_content(msgpack_packer *tmp_pck,
                                 msgpack_object array,
                                 struct flb_elasticsearch *ctx)
{
    int i;
    msgpack_object *e;

    for (i = 0; i < array.via.array.size; i++) {
        e = &array.via.array.ptr[i];
        if (e->type == MSGPACK_OBJECT_MAP)
        {
            msgpack_pack_map(tmp_pck, e->via.map.size);
            es_pack_map_content(tmp_pck, *e, ctx);
        }
        else if (e->type == MSGPACK_OBJECT_ARRAY)
        {
            msgpack_pack_array(tmp_pck, e->via.array.size);
            es_pack_array_content(tmp_pck, *e, ctx);
        }
        else
        {
            msgpack_pack_object(tmp_pck, *e);
        }
    }
    return 0;
}

/*
 * Convert the internal Fluent Bit data representation to the required
 * one by Elasticsearch.
 *
 * 'Sadly' this process involves to convert from Msgpack to JSON.
 */
static int elasticsearch_format(struct flb_config *config,
                                struct flb_input_instance *ins,
                                void *plugin_context,
                                void *flush_ctx,
                                const char *tag, int tag_len,
                                const void *data, size_t bytes,
                                void **out_data, size_t *out_size)
{
    int ret;
    int len;
    int map_size;
    int index_len = 0;
    size_t s = 0;
    size_t off = 0;
    char *p;
    char *es_index;
    char logstash_index[256];
    char time_formatted[256];
    char index_formatted[256];
    char es_uuid[37];
    flb_sds_t out_buf;
    msgpack_unpacked result;
    msgpack_object root;
    msgpack_object map;
    msgpack_object *obj;
    char j_index[ES_BULK_HEADER];
    struct es_bulk *bulk;
    struct tm tm;
    struct flb_time tms;
    msgpack_sbuffer tmp_sbuf;
    msgpack_packer tmp_pck;
    uint16_t hash[8];
    int es_index_custom_len;
    struct flb_elasticsearch *ctx = plugin_context;

    /* Iterate the original buffer and perform adjustments */
    msgpack_unpacked_init(&result);

    /* Perform some format validation */
    ret = msgpack_unpack_next(&result, data, bytes, &off);
    if (ret != MSGPACK_UNPACK_SUCCESS) {
        msgpack_unpacked_destroy(&result);
        return -1;
    }

    /* We 'should' get an array */
    if (result.data.type != MSGPACK_OBJECT_ARRAY) {
        /*
         * If we got a different format, we assume the caller knows what he is
         * doing, we just duplicate the content in a new buffer and cleanup.
         */
        msgpack_unpacked_destroy(&result);
        return -1;
    }

    root = result.data;
    if (root.via.array.size == 0) {
        return -1;
    }

    /* Create the bulk composer */
    bulk = es_bulk_create();
    if (!bulk) {
        return -1;
    }

    off = 0;

    msgpack_unpacked_destroy(&result);
    msgpack_unpacked_init(&result);

    /* Copy logstash prefix if logstash format is enabled */
    if (ctx->logstash_format == FLB_TRUE) {
        memcpy(logstash_index, ctx->logstash_prefix, flb_sds_len(ctx->logstash_prefix));
        logstash_index[flb_sds_len(ctx->logstash_prefix)] = '\0';
    }

    /*
     * If logstash format and id generation are disabled, pre-generate
     * the index line for all records.
     *
     * The header stored in 'j_index' will be used for the all records on
     * this payload.
     */
    if (ctx->logstash_format == FLB_FALSE && ctx->generate_id == FLB_FALSE) {
        flb_time_get(&tms);
        gmtime_r(&tms.tm.tv_sec, &tm);
        strftime(index_formatted, sizeof(index_formatted) - 1,
                 ctx->index, &tm);
        es_index = index_formatted;
        if(ctx->id_format == NULL) {
            if (ctx->suppress_type_name) {
                index_len = snprintf(j_index,
                                    ES_BULK_HEADER,
                                    ES_BULK_INDEX_FMT_WITHOUT_TYPE,
                                    es_index);
            }
            else {
                index_len = snprintf(j_index,
                                    ES_BULK_HEADER,
                                    ES_BULK_INDEX_FMT,
                                    es_index, ctx->type);
            }
        } else {
            flb_plg_debug(ctx->ins, "using id format from configure.. %s", ctx->id_format);
            char * p_id_format = (char *)malloc(300);
            memset(p_id_format, 0, sizeof(p_id_format));
            const char *p  = ctx->id_format, *q = ctx->id_format;
            const char *p_id_formatPtr = p_id_format;
            const char *old_p = p;
            char key[30];
            while(1){
                old_p = p;
                p = strstr(p, "$[");
                if(p == NULL) {
                    if(old_p != NULL) {
                        strcat(p_id_format, old_p);
                    }
                    break;
                }
                if(old_p != NULL) {
                    strncat(p_id_format, old_p, p-old_p);
                }
                q = strstr(p, "]");
                if(p!= NULL && q!= NULL) {
                    memset(key, 0, 30);
                    strncpy(key, p+2, q-p-2);
                    key[q-p-2] = '\0';
                    flb_plg_debug(ctx->ins, "current key is %s ", key);
                    p = ++q;
                    msgpack_object_kv *pKv = map.via.map.ptr;
                    while(pKv++->key.type != MSGPACK_OBJECT_NIL) {
                        // printf("current key type:%d ==> val type %d \n", pKv->key.type, pKv->val.type );
                        if(pKv->key.type == MSGPACK_OBJECT_STR && pKv->val.type == MSGPACK_OBJECT_STR) {
                            if(strncasecmp(pKv->key.via.str.ptr, key, pKv->key.via.str.size) == 0 ){ // Found value
                                strncat(p_id_format, pKv->val.via.str.ptr, pKv->val.via.str.size);
                                printf("current id_format size:%d\n", pKv->val.via.str.size);
                            }
                        }
                    }
                }else{
                    break;
                }
            }
            p_id_format[strlen(p_id_format)] = '\0';
            
            if(ctx->suppress_type_name){
                index_len = snprintf(j_index, 
                                    ES_BULK_CHUNK,
                                    ES_BULK_INDEX_FMT_ID_WITHOUT_TYPE,
                                    es_index, p_id_format
                                    );
            } else {
                index_len = snprintf(j_index,
                                    ES_BULK_HEADER,
                                    ES_BULK_INDEX_FMT_ID,
                                    es_index, ctx->type, p_id_format
                                    );
            }
            free(p_id_format);
        }
    }

    /*
     * Some broken clients may have time drift up to year 1970
     * this will generate corresponding index in Elasticsearch
     * in order to prevent generating millions of indexes
     * we can set to always use current time for index generation
     */
    if (ctx->current_time_index == FLB_TRUE) {
        flb_time_get(&tms);
    }

    /* Iterate each record and do further formatting */
    while (msgpack_unpack_next(&result, data, bytes, &off) == MSGPACK_UNPACK_SUCCESS) {
        if (result.data.type != MSGPACK_OBJECT_ARRAY) {
            continue;
        }

        /* Each array must have two entries: time and record */
        root = result.data;
        if (root.via.array.size != 2) {
            continue;
        }

        /* Only pop time from record if current_time_index is disabled */
        if (ctx->current_time_index == FLB_FALSE) {
            flb_time_pop_from_msgpack(&tms, &result, &obj);
        }

        map   = root.via.array.ptr[1];
        map_size = map.via.map.size;

        es_index_custom_len = 0;
        if (ctx->logstash_prefix_key) {
            flb_sds_t v = flb_ra_translate(ctx->ra_prefix_key,
                                           (char *) tag, tag_len,
                                           map, NULL);
            if (v) {
                len = flb_sds_len(v);
                if (len > 128) {
                    len = 128;
                    memcpy(logstash_index, v, 128);
                }
                else {
                    memcpy(logstash_index, v, len);
                }
                es_index_custom_len = len;
                flb_sds_destroy(v);
            }
        }

        /* Create temporary msgpack buffer */
        msgpack_sbuffer_init(&tmp_sbuf);
        msgpack_packer_init(&tmp_pck, &tmp_sbuf, msgpack_sbuffer_write);

        if (ctx->include_tag_key == FLB_TRUE) {
            map_size++;
        }

        /* Set the new map size */
        msgpack_pack_map(&tmp_pck, map_size + 1);

        /* Append the time key */
        msgpack_pack_str(&tmp_pck, flb_sds_len(ctx->time_key));
        msgpack_pack_str_body(&tmp_pck, ctx->time_key, flb_sds_len(ctx->time_key));

        /* Format the time */
        gmtime_r(&tms.tm.tv_sec, &tm);
        s = strftime(time_formatted, sizeof(time_formatted) - 1,
                     ctx->time_key_format, &tm);
        if (ctx->time_key_nanos) {
            len = snprintf(time_formatted + s, sizeof(time_formatted) - 1 - s,
                           ".%09" PRIu64 "Z", (uint64_t) tms.tm.tv_nsec);
        } else {
            len = snprintf(time_formatted + s, sizeof(time_formatted) - 1 - s,
                           ".%03" PRIu64 "Z",
                           (uint64_t) tms.tm.tv_nsec / 1000000);
        }

        s += len;
        msgpack_pack_str(&tmp_pck, s);
        msgpack_pack_str_body(&tmp_pck, time_formatted, s);

        es_index = ctx->index;
        if (ctx->logstash_format == FLB_TRUE) {
            /* Compose Index header */
            if (es_index_custom_len > 0) {
                p = logstash_index + es_index_custom_len;
            } else {
                p = logstash_index + flb_sds_len(ctx->logstash_prefix);
            }
            *p++ = '-';

            len = p - logstash_index;
            s = strftime(p, sizeof(logstash_index) - len - 1,
                         ctx->logstash_dateformat, &tm);
            p += s;
            *p++ = '\0';
            es_index = logstash_index;
            if (ctx->generate_id == FLB_FALSE) {
                if (ctx->suppress_type_name) {
                    index_len = snprintf(j_index,
                                         ES_BULK_HEADER,
                                         ES_BULK_INDEX_FMT_WITHOUT_TYPE,
                                         es_index);
                }
                else {
                    index_len = snprintf(j_index,
                                         ES_BULK_HEADER,
                                         ES_BULK_INDEX_FMT,
                                         es_index, ctx->type);
                }
            }
        }
        else if (ctx->current_time_index == FLB_TRUE) {
            /* Make sure we handle index time format for index */
            strftime(index_formatted, sizeof(index_formatted) - 1,
                     ctx->index, &tm);
            es_index = index_formatted;
        }

        /* Tag Key */
        if (ctx->include_tag_key == FLB_TRUE) {
            msgpack_pack_str(&tmp_pck, flb_sds_len(ctx->tag_key));
            msgpack_pack_str_body(&tmp_pck, ctx->tag_key, flb_sds_len(ctx->tag_key));
            msgpack_pack_str(&tmp_pck, tag_len);
            msgpack_pack_str_body(&tmp_pck, tag, tag_len);
        }

        /*
         * The map_content routine iterate over each Key/Value pair found in
         * the map and do some sanitization for the key names.
         *
         * Elasticsearch have a restriction that key names cannot contain
         * a dot; if some dot is found, it's replaced with an underscore.
         */
        ret = es_pack_map_content(&tmp_pck, map, ctx);
        if (ret == -1) {
            msgpack_unpacked_destroy(&result);
            msgpack_sbuffer_destroy(&tmp_sbuf);
            es_bulk_destroy(bulk);
            return -1;
        }

        if (ctx->generate_id == FLB_TRUE) {
            MurmurHash3_x64_128(tmp_sbuf.data, tmp_sbuf.size, 42, hash);
            snprintf(es_uuid, sizeof(es_uuid),
                     "%04x%04x-%04x-%04x-%04x-%04x%04x%04x",
                     hash[0], hash[1], hash[2], hash[3],
                     hash[4], hash[5], hash[6], hash[7]);
            if (ctx->suppress_type_name) {
                index_len = snprintf(j_index,
                                     ES_BULK_HEADER,
                                     ES_BULK_INDEX_FMT_ID_WITHOUT_TYPE,
                                     es_index,  es_uuid);
            }
            else {
                index_len = snprintf(j_index,
                                     ES_BULK_HEADER,
                                     ES_BULK_INDEX_FMT_ID,
                                     es_index, ctx->type, es_uuid);
            }
        }

        /* Convert msgpack to JSON */
        out_buf = flb_msgpack_raw_to_json_sds(tmp_sbuf.data, tmp_sbuf.size);
        msgpack_sbuffer_destroy(&tmp_sbuf);
        if (!out_buf) {
            msgpack_unpacked_destroy(&result);
            es_bulk_destroy(bulk);
            return -1;
        }

        ret = es_bulk_append(bulk, j_index, index_len,
                             out_buf, flb_sds_len(out_buf));
        flb_sds_destroy(out_buf);
        if (ret == -1) {
            /* We likely ran out of memory, abort here */
            msgpack_unpacked_destroy(&result);
            *out_size = 0;
            es_bulk_destroy(bulk);
            return -1;
        }
    }
    msgpack_unpacked_destroy(&result);

    /* Set outgoing data */
    *out_data = bulk->ptr;
    *out_size = bulk->len;

    /*
     * Note: we don't destroy the bulk as we need to keep the allocated
     * buffer with the data. Instead we just release the bulk context and
     * return the bulk->ptr buffer
     */
    flb_free(bulk);
    if (ctx->trace_output) {
        fwrite(*out_data, 1, *out_size, stdout);
        fflush(stdout);
    }

    return 0;
}

static int cb_es_init(struct flb_output_instance *ins,
                      struct flb_config *config,
                      void *data)
{
    struct flb_elasticsearch *ctx;

    ctx = flb_es_conf_create(ins, config);
    if (!ctx) {
        flb_plg_error(ins, "cannot initialize plugin");
        return -1;
    }

    flb_plg_debug(ctx->ins, "host=%s port=%i uri=%s index=%s type=%s",
                  ins->host.name, ins->host.port, ctx->uri,
                  ctx->index, ctx->type);

    flb_output_set_context(ins, ctx);

    /*
     * This plugin instance uses the HTTP client interface, let's register
     * it debugging callbacks.
     */
    flb_output_set_http_debug_callbacks(ins);

    return 0;
}

static int elasticsearch_error_check(struct flb_elasticsearch *ctx,
                                     struct flb_http_client *c)
{
    int i;
    int ret;
    int check = FLB_TRUE;
    int root_type;
    char *out_buf;
    size_t off = 0;
    size_t out_size;
    msgpack_unpacked result;
    msgpack_object root;
    msgpack_object key;
    msgpack_object val;

    /*
     * Check if our payload is complete: there is such situations where
     * the Elasticsearch HTTP response body is bigger than the HTTP client
     * buffer so payload can be incomplete.
     */
    /* Convert JSON payload to msgpack */
    ret = flb_pack_json(c->resp.payload, c->resp.payload_size,
                        &out_buf, &out_size, &root_type);
    if (ret == -1) {
        /* Is this an incomplete HTTP Request ? */
        if (c->resp.payload_size <= 0) {
            return FLB_TRUE;
        }

        /* Lookup error field */
        if (strstr(c->resp.payload, "\"errors\":false,\"items\":[")) {
            return FLB_FALSE;
        }

        flb_plg_error(ctx->ins, "could not pack/validate JSON response\n%s",
                      c->resp.payload);
        return FLB_TRUE;
    }

    /* Lookup error field */
    msgpack_unpacked_init(&result);
    ret = msgpack_unpack_next(&result, out_buf, out_size, &off);
    if (ret != MSGPACK_UNPACK_SUCCESS) {
        flb_plg_error(ctx->ins, "Cannot unpack response to find error\n%s",
                      c->resp.payload);
        return FLB_TRUE;
    }

    root = result.data;
    if (root.type != MSGPACK_OBJECT_MAP) {
        flb_plg_error(ctx->ins, "unexpected payload type=%i",
                      root.type);
        check = FLB_TRUE;
        goto done;
    }

    for (i = 0; i < root.via.map.size; i++) {
        key = root.via.map.ptr[i].key;
        if (key.type != MSGPACK_OBJECT_STR) {
            flb_plg_error(ctx->ins, "unexpected key type=%i",
                          key.type);
            check = FLB_TRUE;
            goto done;
        }

        if (key.via.str.size != 6) {
            continue;
        }

        if (strncmp(key.via.str.ptr, "errors", 6) == 0) {
            val = root.via.map.ptr[i].val;
            if (val.type != MSGPACK_OBJECT_BOOLEAN) {
                flb_plg_error(ctx->ins, "unexpected 'error' value type=%i",
                              val.type);
                check = FLB_TRUE;
                goto done;
            }

            /* If error == false, we are OK (no errors = FLB_FALSE) */
            if (val.via.boolean) {
                /* there is an error */
                check = FLB_TRUE;
                goto done;
            }
            else {
                /* no errors */
                check = FLB_FALSE;
                goto done;
            }
        }
    }

 done:
    flb_free(out_buf);
    msgpack_unpacked_destroy(&result);
    return check;
}

static void cb_es_flush(const void *data, size_t bytes,
                        const char *tag, int tag_len,
                        struct flb_input_instance *ins, void *out_context,
                        struct flb_config *config)
{
    int ret;
    size_t pack_size;
    char *pack;
    void *out_buf;
    size_t out_size;
    size_t b_sent;
    struct flb_elasticsearch *ctx = out_context;
    struct flb_upstream_conn *u_conn;
    struct flb_http_client *c;
    flb_sds_t signature = NULL;

    /* Get upstream connection */
    u_conn = flb_upstream_conn_get(ctx->u);
    if (!u_conn) {
        FLB_OUTPUT_RETURN(FLB_RETRY);
    }

    /* Convert format */
    ret = elasticsearch_format(config, ins,
                               ctx, NULL,
                               tag, tag_len,
                               data, bytes,
                               &out_buf, &out_size);
    if (ret != 0) {
        flb_upstream_conn_release(u_conn);
        FLB_OUTPUT_RETURN(FLB_ERROR);
    }

    pack = (char *) out_buf;
    pack_size = out_size;

    /* Compose HTTP Client request */
    c = flb_http_client(u_conn, FLB_HTTP_POST, ctx->uri,
                        pack, pack_size, NULL, 0, NULL, 0);

    flb_http_buffer_size(c, ctx->buffer_size);

#ifndef FLB_HAVE_AWS
    flb_http_add_header(c, "User-Agent", 10, "Fluent-Bit", 10);
#endif

    flb_http_add_header(c, "Content-Type", 12, "application/x-ndjson", 20);

    if (ctx->http_user && ctx->http_passwd) {
        flb_http_basic_auth(c, ctx->http_user, ctx->http_passwd);
    }
    else if (ctx->cloud_user && ctx->cloud_passwd) {
        flb_http_basic_auth(c, ctx->cloud_user, ctx->cloud_passwd);
    }

#ifdef FLB_HAVE_AWS
    if (ctx->has_aws_auth == FLB_TRUE) {
        signature = add_aws_auth(c, ctx);
        if (!signature) {
            goto retry;
        }
    }
    else {
        flb_http_add_header(c, "User-Agent", 10, "Fluent-Bit", 10);
    }
#endif

    /* Map debug callbacks */
    flb_http_client_debug(c, ctx->ins->callback);

    ret = flb_http_do(c, &b_sent);
    if (ret != 0) {
        flb_plg_warn(ctx->ins, "http_do=%i URI=%s", ret, ctx->uri);
        goto retry;
    }
    else {
        /* The request was issued successfully, validate the 'error' field */
        flb_plg_debug(ctx->ins, "HTTP Status=%i URI=%s", c->resp.status, ctx->uri);
        if (c->resp.status != 200 && c->resp.status != 201) {
            if (c->resp.payload_size > 0) {
                flb_plg_error(ctx->ins, "HTTP status=%i URI=%s, response:\n%s\n",
                              c->resp.status, ctx->uri, c->resp.payload);
            }
            else {
                flb_plg_error(ctx->ins, "HTTP status=%i URI=%s",
                              c->resp.status, ctx->uri);
            }
            goto retry;
        }

        if (c->resp.payload_size > 0) {
            /*
             * Elasticsearch payload should be JSON, we convert it to msgpack
             * and lookup the 'error' field.
             */
            ret = elasticsearch_error_check(ctx, c);
            if (ret == FLB_TRUE) {
                /* we got an error */
                if (ctx->trace_error) {
                    /*
                     * If trace_error is set, trace the actual
                     * input/output to Elasticsearch that caused the problem.
                     */
                    flb_plg_debug(ctx->ins, "error caused by: Input\n%s\n",
                                  pack);
                    flb_plg_error(ctx->ins, "error: Output\n%s",
                                  c->resp.payload);
                }
                goto retry;
            }
            else {
                flb_plg_debug(ctx->ins, "Elasticsearch response\n%s",
                              c->resp.payload);
            }
        }
        else {
            goto retry;
        }
    }

    /* Cleanup */
    flb_http_client_destroy(c);
    flb_free(pack);
    flb_upstream_conn_release(u_conn);
    if (signature) {
        flb_sds_destroy(signature);
    }
    FLB_OUTPUT_RETURN(FLB_OK);

    /* Issue a retry */
 retry:
    flb_http_client_destroy(c);
    flb_free(pack);
    flb_upstream_conn_release(u_conn);
    FLB_OUTPUT_RETURN(FLB_RETRY);
}

static int cb_es_exit(void *data, struct flb_config *config)
{
    struct flb_elasticsearch *ctx = data;

    flb_es_conf_destroy(ctx);
    return 0;
}

/* Configuration properties map */
static struct flb_config_map config_map[] = {
    {
     FLB_CONFIG_MAP_STR, "index", FLB_ES_DEFAULT_INDEX,
     0, FLB_TRUE, offsetof(struct flb_elasticsearch, index),
     "Set an index name"
    },
    {
     FLB_CONFIG_MAP_STR, "type", FLB_ES_DEFAULT_TYPE,
     0, FLB_TRUE, offsetof(struct flb_elasticsearch, type),
     "Set the document type property"
    },
    {
     FLB_CONFIG_MAP_BOOL, "suppress_type_name", "false",
     0, FLB_TRUE, offsetof(struct flb_elasticsearch, suppress_type_name),
     "If true, mapping types is removed. (for v7.0.0 or later)"
    },

    /* HTTP Authentication */
    {
     FLB_CONFIG_MAP_STR, "http_user", NULL,
     0, FLB_TRUE, offsetof(struct flb_elasticsearch, http_user),
     "Optional username credential for Elastic X-Pack access"
    },
    {
     FLB_CONFIG_MAP_STR, "http_passwd", "",
     0, FLB_TRUE, offsetof(struct flb_elasticsearch, http_passwd),
     "Password for user defined in HTTP_User"
    },

    /* Cloud Authentication */
    {
     FLB_CONFIG_MAP_STR, "cloud_id", NULL,
     0, FLB_FALSE, 0,
     "Elastic cloud ID of the cluster to connect to"
    },
    {
     FLB_CONFIG_MAP_STR, "cloud_auth", NULL,
     0, FLB_FALSE, 0,
     "Elastic cloud authentication credentials"
    },

    /* AWS Authentication */
#ifdef FLB_HAVE_AWS
    {
     FLB_CONFIG_MAP_BOOL, "aws_auth", "false",
     0, FLB_TRUE, offsetof(struct flb_elasticsearch, has_aws_auth),
     "Enable AWS Sigv4 Authentication"
    },
    {
     FLB_CONFIG_MAP_STR, "aws_region", NULL,
     0, FLB_TRUE, offsetof(struct flb_elasticsearch, aws_region),
     "AWS Region of your Amazon ElasticSearch Service cluster"
    },
    {
     FLB_CONFIG_MAP_STR, "aws_sts_endpoint", NULL,
     0, FLB_TRUE, offsetof(struct flb_elasticsearch, aws_sts_endpoint),
     "Custom endpoint for the AWS STS API, used with the AWS_Role_ARN option"
    },
    {
     FLB_CONFIG_MAP_STR, "aws_role_arn", NULL,
     0, FLB_FALSE, 0,
     "AWS IAM Role to assume to put records to your Amazon ES cluster"
    },
    {
     FLB_CONFIG_MAP_STR, "aws_external_id", NULL,
     0, FLB_FALSE, 0,
     "External ID for the AWS IAM Role specified with `aws_role_arn`"
    },
#endif

    /* Logstash compatibility */
    {
     FLB_CONFIG_MAP_BOOL, "logstash_format", "false",
     0, FLB_TRUE, offsetof(struct flb_elasticsearch, logstash_format),
     "Enable Logstash format compatibility"
    },
    {
     FLB_CONFIG_MAP_STR, "logstash_prefix", FLB_ES_DEFAULT_PREFIX,
     0, FLB_TRUE, offsetof(struct flb_elasticsearch, logstash_prefix),
     "When Logstash_Format is enabled, the Index name is composed using a prefix "
     "and the date, e.g: If Logstash_Prefix is equals to 'mydata' your index will "
     "become 'mydata-YYYY.MM.DD'. The last string appended belongs to the date "
     "when the data is being generated"
    },
    {
     FLB_CONFIG_MAP_STR, "logstash_prefix_key", NULL,
     0, FLB_TRUE, offsetof(struct flb_elasticsearch, logstash_prefix_key),
     "When included: the value in the record that belongs to the key will be looked "
     "up and over-write the Logstash_Prefix for index generation. If the key/value "
     "is not found in the record then the Logstash_Prefix option will act as a "
     "fallback. Nested keys are supported through record accessor pattern"
    },
    {
     FLB_CONFIG_MAP_STR, "logstash_dateformat", FLB_ES_DEFAULT_TIME_FMT,
     0, FLB_TRUE, offsetof(struct flb_elasticsearch, logstash_dateformat),
     "Time format (based on strftime) to generate the second part of the Index name"
    },

    /* Custom Time and Tag keys */
    {
     FLB_CONFIG_MAP_STR, "time_key", FLB_ES_DEFAULT_TIME_KEY,
     0, FLB_TRUE, offsetof(struct flb_elasticsearch, time_key),
     "When Logstash_Format is enabled, each record will get a new timestamp field. "
     "The Time_Key property defines the name of that field"
    },
    {
     FLB_CONFIG_MAP_STR, "time_key_format", FLB_ES_DEFAULT_TIME_KEYF,
     0, FLB_TRUE, offsetof(struct flb_elasticsearch, time_key_format),
     "When Logstash_Format is enabled, this property defines the format of the "
     "timestamp"
    },
    {
     FLB_CONFIG_MAP_BOOL, "time_key_nanos", "false",
     0, FLB_TRUE, offsetof(struct flb_elasticsearch, time_key_nanos),
     "When Logstash_Format is enabled, enabling this property sends nanosecond "
     "precision timestamps"
    },
    {
     FLB_CONFIG_MAP_BOOL, "include_tag_key", "false",
     0, FLB_TRUE, offsetof(struct flb_elasticsearch, include_tag_key),
     "When enabled, it append the Tag name to the record"
    },
    {
     FLB_CONFIG_MAP_STR, "tag_key", FLB_ES_DEFAULT_TAG_KEY,
     0, FLB_TRUE, offsetof(struct flb_elasticsearch, tag_key),
     "When Include_Tag_Key is enabled, this property defines the key name for the tag"
    },
    {
     FLB_CONFIG_MAP_SIZE, "buffer_size", FLB_ES_DEFAULT_HTTP_MAX,
     0, FLB_TRUE, offsetof(struct flb_elasticsearch, buffer_size),
     "Specify the buffer size used to read the response from the Elasticsearch HTTP "
     "service. This option is useful for debugging purposes where is required to read "
     "full responses, note that response size grows depending of the number of records "
     "inserted. To set an unlimited amount of memory set this value to 'false', "
     "otherwise the value must be according to the Unit Size specification"
    },

    /* Elasticsearch specifics */
    {
     FLB_CONFIG_MAP_STR, "path", NULL,
     0, FLB_FALSE, 0,
     "Elasticsearch accepts new data on HTTP query path '/_bulk'. But it is also "
     "possible to serve Elasticsearch behind a reverse proxy on a subpath. This "
     "option defines such path on the fluent-bit side. It simply adds a path "
     "prefix in the indexing HTTP POST URI"
    },
    {
     FLB_CONFIG_MAP_STR, "pipeline", NULL,
     0, FLB_FALSE, 0,
     "Newer versions of Elasticsearch allows to setup filters called pipelines. "
     "This option allows to define which pipeline the database should use. For "
     "performance reasons is strongly suggested to do parsing and filtering on "
     "Fluent Bit side, avoid pipelines"
    },
    {
     FLB_CONFIG_MAP_BOOL, "generate_id", "false",
     0, FLB_TRUE, offsetof(struct flb_elasticsearch, generate_id),
     "When enabled, generate _id for outgoing records. This prevents duplicate "
     "records when retrying ES"
    },
    {
     FLB_CONFIG_MAP_BOOL, "replace_dots", "false",
     0, FLB_TRUE, offsetof(struct flb_elasticsearch, replace_dots),
     "When enabled, replace field name dots with underscore, required by Elasticsearch "
     "2.0-2.3."
    },
    {
     FLB_CONFIG_MAP_STR, "id_format", NULL, 
     0, FLB_FALSE, offsetof(struct flb_elasticsearch, id_format),
     "When enabled, the es [_id] would may format like this. This may dupliacte " 
     "records when retrying ES. format it carefully and current support the the "
     "first layer"
    },
    {
     FLB_CONFIG_MAP_BOOL, "current_time_index", "false",
     0, FLB_TRUE, offsetof(struct flb_elasticsearch, current_time_index),
     "Use current time for index generation instead of message record"
    },

    /* Trace */
    {
     FLB_CONFIG_MAP_BOOL, "trace_output", "false",
     0, FLB_TRUE, offsetof(struct flb_elasticsearch, trace_output),
     "When enabled print the Elasticsearch API calls to stdout (for diag only)"
    },
    {
     FLB_CONFIG_MAP_BOOL, "trace_error", "false",
     0, FLB_TRUE, offsetof(struct flb_elasticsearch, trace_error),
     "When enabled print the Elasticsearch exception to stderr (for diag only)"
    },

    /* EOF */
    {0}
};

/* Plugin reference */
struct flb_output_plugin out_es_plugin = {
    .name           = "es",
    .description    = "Elasticsearch",
    .cb_init        = cb_es_init,
    .cb_pre_run     = NULL,
    .cb_flush       = cb_es_flush,
    .cb_exit        = cb_es_exit,

    /* Configuration */
    .config_map     = config_map,

    /* Test */
    .test_formatter.callback = elasticsearch_format,

    /* Plugin flags */
    .flags          = FLB_OUTPUT_NET | FLB_IO_OPT_TLS,
};
