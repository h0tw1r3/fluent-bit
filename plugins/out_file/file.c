/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */

/*  Fluent Bit
 *  ==========
 *  Copyright (C) 2015-2017 Treasure Data Inc.
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

#include <fluent-bit/flb_mem.h>
#include <fluent-bit/flb_output.h>
#include <fluent-bit/flb_pack.h>
#include <fluent-bit/flb_utils.h>
#include <fluent-bit/flb_time.h>
#include <msgpack.h>

#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#include "file.h"

struct flb_file_conf {
    char *out_file;
    char *delimiter;
    int  format;
};

static int cb_file_init(struct flb_output_instance *ins,
                        struct flb_config *config,
                        void *data)
{
    char *tmp;
    (void) config;
    (void) data;
    struct flb_file_conf *conf;

    conf = flb_calloc(1, sizeof(struct flb_file_conf));
    if (!conf) {
        flb_errno();
        return -1;
    }

    conf->format = FLB_OUT_FILE_FMT_JSON;/* default */
    conf->delimiter = ",";/* default */

    /* Optional output file name/path */
    tmp = flb_output_get_property("Path", ins);
    if (tmp) {
        conf->out_file = tmp;
    }

    /* Optional, file format */
    tmp = flb_output_get_property("Format", ins);
    if (tmp && !strcasecmp(tmp, "csv")){
        conf->format = FLB_OUT_FILE_FMT_CSV;
    }

    tmp = flb_output_get_property("delimiter", ins);
    if (tmp && !strcasecmp(tmp, "\\t")) {
        conf->delimiter = "\t";
    }
    else if (tmp) {
        conf->delimiter = tmp;
    }

    /* Set the context */
    flb_output_set_context(ins, conf);

    return 0;
}

static int csv_output(FILE *fp,
                      struct flb_time *tm,
                      msgpack_object *obj,
                      struct flb_file_conf *ctx)
{
    msgpack_object_kv *kv = NULL;
    int i;
    int map_size;

    if (obj->type == MSGPACK_OBJECT_MAP && obj->via.map.size > 0) {
        kv = obj->via.map.ptr;
        map_size = obj->via.map.size;
        fprintf(fp, "%f%s", flb_time_to_double(tm), ctx->delimiter);
        for (i=0; i<map_size-1; i++) {
            msgpack_object_print(fp, (kv+i)->val);
            fprintf(fp, "%s", ctx->delimiter);
        }
        msgpack_object_print(fp, (kv+(map_size-1))->val);
        fprintf(fp, "\n");
    }
    return 0;
}

static void cb_file_flush(void *data, size_t bytes,
                          char *tag, int tag_len,
                          struct flb_input_instance *i_ins,
                          void *out_context,
                          struct flb_config *config)
{
    FILE * fp;
    msgpack_unpacked result;
    size_t off = 0;
    size_t last_off = 0;
    size_t alloc_size = 0;
    char *out_file;
    char *buf;
    msgpack_object *obj;
    struct flb_file_conf *ctx = out_context;
    struct flb_time tm;
    (void) i_ins;
    (void) config;

    /* Set the right output */
    if (!ctx->out_file) {
        out_file = tag;
    }
    else {
        out_file = ctx->out_file;
    }

    /* Open output file with default name as the Tag */
    fp = fopen(out_file, "a+");
    if (fp == NULL) {
        flb_errno();
        FLB_OUTPUT_RETURN(FLB_ERROR);
    }

    /*
     * Upon flush, for each array, lookup the time and the first field
     * of the map to use as a data point.
     */
    msgpack_unpacked_init(&result);
    while (msgpack_unpack_next(&result, data, bytes, &off)) {
        alloc_size = (off - last_off) + 128;/* JSON is larger than msgpack */
        last_off = off;
        buf = (char *)flb_calloc(1, alloc_size);
        if (buf == NULL) {
            flb_errno();
            msgpack_unpacked_destroy(&result);
            fclose(fp);
            FLB_OUTPUT_RETURN(FLB_RETRY);
        }

        flb_time_pop_from_msgpack(&tm, &result, &obj);
        switch (ctx->format){
        case FLB_OUT_FILE_FMT_JSON: 
            if (flb_msgpack_obj_to_json(buf, alloc_size, obj) >= 0) {
                fprintf(fp, "%s: [%f, %s]\n",
                        tag,
                        flb_time_to_double(&tm),
                        buf);
            }
            break;
        case FLB_OUT_FILE_FMT_CSV:
            csv_output(fp, &tm, obj, ctx);
            break;
        }
        flb_free(buf);
    }
    msgpack_unpacked_destroy(&result);

    fclose(fp);

    FLB_OUTPUT_RETURN(FLB_OK);
}

static int cb_file_exit(void *data, struct flb_config *config)
{
    struct flb_file_conf *ctx = data;

    flb_free(ctx);

    return 0;
}

struct flb_output_plugin out_file_plugin = {
    .name         = "file",
    .description  = "Generate log file",
    .cb_init      = cb_file_init,
    .cb_flush     = cb_file_flush,
    .cb_exit      = cb_file_exit,
    .flags        = 0,
};
