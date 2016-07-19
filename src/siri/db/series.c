/*
 * series.c - Series
 *
 * author       : Jeroen van der Heijden
 * email        : jeroen@transceptor.technology
 * copyright    : 2016, Transceptor Technology
 *
 * changes
 *  - initial version, 29-03-2016
 *
 */
#include <siri/db/series.h>
#include <stdlib.h>
#include <logger/logger.h>
#include <siri/db/db.h>
#include <siri/db/buffer.h>
#include <stdio.h>
#include <assert.h>
#include <string.h>
#include <siri/db/shard.h>
#include <siri/siri.h>
#include <xpath/xpath.h>

#define SIRIDB_SERIES_FN "series.dat"
#define SIRIDB_DROPPED_FN ".dropped"
#define SIRIDB_MAX_SERIES_ID_FN ".max_series_id"
#define SIRIDB_REPLICATE_FN ".replicate"
#define SIRIDB_SERIES_SCHEMA 1
#define BEND series->buffer->points->data[series->buffer->points->len - 1].ts
#define DROPPED_DUMMY 1

static int SERIES_save(siridb_t * siridb);
static void SERIES_free(siridb_series_t * series);
static int SERIES_load(siridb_t * siridb, imap32_t * dropped);
static int SERIES_read_dropped(siridb_t * siridb, imap32_t * dropped);
static int SERIES_open_new_dropped_file(siridb_t * siridb);
static int SERIES_open_store(siridb_t * siridb);
static int SERIES_update_max_id(siridb_t * siridb);
static void SERIES_update_start_num32(siridb_series_t * series);
static void SERIES_update_end_num32(siridb_series_t * series);



static int SERIES_pack(
        const char * key,
        siridb_series_t * series,
        qp_fpacker_t * fpacker);

static siridb_series_t * SERIES_new(
        siridb_t * siridb,
        uint32_t id,
        uint8_t tp,
        const char * sn);

const char series_type_map[3][8] = {
        "integer",
        "float",
        "string"
};

/*
 * Call-back used to compare series properties.
 *
 * Returns 1 when series match or 0 if not.
 */
int siridb_series_cexpr_cb(
        siridb_series_walker_t * wseries,
        cexpr_condition_t * cond)
{
    switch (cond->prop)
    {
    case CLERI_GID_K_LENGTH:
        return cexpr_int_cmp(cond->operator, wseries->series->length, cond->int64);
    case CLERI_GID_K_START:
        return cexpr_int_cmp(cond->operator, wseries->series->start, cond->int64);
    case CLERI_GID_K_END:
        return cexpr_int_cmp(cond->operator, wseries->series->end, cond->int64);
    case CLERI_GID_K_POOL:
        return cexpr_int_cmp(cond->operator, wseries->pool, cond->int64);
    case CLERI_GID_K_TYPE:
        return cexpr_int_cmp(cond->operator, wseries->series->tp, cond->int64);
    case CLERI_GID_K_NAME:
        return cexpr_str_cmp(cond->operator, wseries->series_name, cond->str);
    }
    /* we must NEVER get here */
    log_critical("Unexpected series property received: %d", cond->prop);
    assert (0);
    return -1;
}

/*
 * Write all series id's to the initial replicate file.
 *
 * Returns 0 if successful or some other value if not.
 */
int siridb_series_replicate_file(siridb_t * siridb)
{
    FILE * fp;

    SIRIDB_GET_FN(fn, SIRIDB_REPLICATE_FN)

    fp = fopen(fn, "w");
    if (fp == NULL)
    {
        return -1;
    }
    int rc = imap32_walk(
            siridb->series_map,
            (imap32_cb) SERIES_create_repl_cb,
            fp);

    if (fclose(fp))
    {
        rc = -1;
    }
    return rc;
}

/*
 * Typedef: imap32_cb
 *
 * Returns 0 if successful
 */
static int SERIES_create_repl_cb(siridb_series_t * series, FILE * fp)
{
    return fwrite(series->id, sizeof(uint32_t), 1, fp) - 1;
}

/*
 * Returns 0 if successful; -1 and a SIGNAL is raised in case an error occurred.
 *
 * Warning:
 *      do not call this function after a siri_err has occurred since this
 *      would risk a stack overflow in case this series has caused the siri_err
 *      and the buffer length is not reset.
 */
int siridb_series_add_point(
        siridb_t * siridb,
        siridb_series_t * series,
        uint64_t * ts,
        qp_via_t * val)
{
#ifdef DEBUG
    assert (!siri_err);
#endif
    int rc = 0;

    series->length++;
    if (*ts < series->start)
    {
        series->start = *ts;
    }
    if (*ts > series->end)
    {
        series->end = *ts;
    }
    if (series->buffer != NULL)
    {
        /* add point in memory
         * (memory can hold 1 more point than we can hold on disk)
         */
        siridb_points_add_point(series->buffer->points, ts, val);

        if (series->buffer->points->len == siridb->buffer_len)
        {
            if (siridb_buffer_to_shards(siridb, series))
            {
                rc = -1;  /* signal is raised */
            }
            else
            {
                series->buffer->points->len = 0;
                if (siridb_buffer_write_len(siridb, series))
                {
                    ERR_FILE
                    rc = -1;
                }
            }
        }
        else
        {
            if (siridb_buffer_write_point(siridb, series, ts, val))
            {
                ERR_FILE
                log_critical("Cannot write new point to buffer");
                rc = -1;
            }
        }
    }
    return rc;
}

/*
 * Returns NULL and raises a SIGNAL in case an error has occurred.
 */
siridb_series_t * siridb_series_new(
        siridb_t * siridb,
        const char * series_name,
        uint8_t tp)
{
    siridb_series_t * series;
    size_t len = strlen(series_name);

    siridb->max_series_id++;

    series = SERIES_new(siridb, siridb->max_series_id, tp, series_name);

    if (series != NULL)
    {
        /* add series to the store */
        if (
                qp_fadd_type(siridb->store, QP_ARRAY3) ||
                qp_fadd_raw(siridb->store, series_name, len + 1) ||
                qp_fadd_int32(siridb->store, (int32_t) series->id) ||
                qp_fadd_int8(siridb->store, (int8_t) series->tp) ||
                qp_flush(siridb->store))
        {
            ERR_FILE
            log_critical("Cannot write series '%s' to store.", series_name);
            SERIES_free(series);
            series = NULL;
        }
        /* create a buffer for series (except string series) */
        else if (
                tp != SIRIDB_SERIES_TP_STRING &&
                siridb_buffer_new_series(siridb, series))
        {
            /* signal is raised */
            log_critical("Could not create buffer for series '%s'.",
                    series_name);
            SERIES_free(series);
            series = NULL;
        }
        /* We only should add the series to series_map and assume the caller
         * takes responsibility adding the series to SiriDB -> series
         */
        else
        {
            imap32_add(siridb->series_map, series->id, series);
        }
    }
    return series;
}

/*
 * Returns 0 if successful or -1 in case or an error.
 * (a SIGNAL might be raised)
 */
int siridb_series_load(siridb_t * siridb)
{
    imap32_t * dropped = imap32_new();
    if (dropped == NULL)
    {
        return -1;
    }

    if (SERIES_read_dropped(siridb, dropped))
    {
        imap32_free(dropped);
        return -1;
    }

    if (SERIES_load(siridb, dropped))
    {
        imap32_free(dropped);
        return -1;
    }

    imap32_free(dropped);

    if (SERIES_update_max_id(siridb))
    {
        return -1;
    }

    if (SERIES_open_new_dropped_file(siridb))
    {
        return -1;
    }

    if (SERIES_open_store(siridb))
    {
        return -1;
    }

    return 0;
}

/*
 * This function should only be called when new values are added.
 * For example, during optimization we do not use this function for
 * replacing indexes. This way we can set the HAS_NEW_VALUES correctly.
 *
 * Returns 0 if successful; -1 and a SIGNAL is raised in case an error occurred.
 */
int siridb_series_add_idx_num32(
        siridb_series_idx_t * index,
        siridb_shard_t * shard,
        uint32_t start_ts,
        uint32_t end_ts,
        uint32_t pos,
        uint16_t len)
{
    idx_num32_t * idx;
    uint32_t i = index->len;
    index->len++;

    idx = (idx_num32_t *) realloc(
            (idx_num32_t *) index->idx,
            index->len * sizeof(idx_num32_t));
    if (idx == NULL)
    {
        ERR_ALLOC
        index->len--;
        return -1;
    }
    index->idx = idx;

    for (; i && start_ts < ((idx_num32_t *) (index->idx))[i - 1].start_ts; i--)
    {
        ((idx_num32_t *) index->idx)[i] = ((idx_num32_t *) index->idx)[i - 1];
    }

    idx = ((idx_num32_t *) (index->idx)) + i;

    /* Check here for new values since we now can compare the current
     * idx->shard with shard. We only set NEW_VALUES when we already have
     * data for this series in the shard and when not loading and not set
     * already. (NEW_VALUES is used for optimize detection and there is
     * nothing to optimize if this is the fist data for this series inside
     * the shard.
     */
    if (    ((shard->flags &
                (SIRIDB_SHARD_HAS_NEW_VALUES |
                        SIRIDB_SHARD_IS_LOADING)) == 0) &&
            ((i > 0 && ((idx_num32_t *) (index->idx))[i - 1].shard == shard) ||
            (i < index->len - 1 && idx->shard == shard)))
    {
        shard->flags |= SIRIDB_SHARD_HAS_NEW_VALUES;
        siridb_shard_write_flags(shard);
    }

    idx->start_ts = start_ts;
    idx->end_ts = end_ts;
    idx->len = len;
    idx->shard = shard;
    idx->pos = pos;

    /* We do not have to save an overlap since it will be detected again when
     * reading the shard at startup.
     */
    if (    (i > 0 &&
            start_ts < ((idx_num32_t *) (index->idx))[i - 1].start_ts) ||
            (++i < index->len &&
            end_ts > ((idx_num32_t *) (index->idx))[i].start_ts))
    {
        shard->flags |= SIRIDB_SHARD_HAS_OVERLAP;
        index->has_overlap = 1;
    }

    return 0;
}

void siridb_series_remove_shard_num32(
        siridb_t * siridb,
        siridb_series_t * series,
        siridb_shard_t * shard)
{
#ifdef DEBUG
    assert (shard->id % siridb->duration_num == series->mask);
#endif

    idx_num32_t * idx;
    uint_fast32_t i, offset;

    i = offset = 0;

    for (   idx = (idx_num32_t *) series->index->idx;
            i < series->index->len;
            i++, idx++)
    {
        if (idx->shard == shard)
        {
            offset++;
            series->length -= idx->len;
        }
        else if (offset)
        {
            ((idx_num32_t *) series->index->idx)[i - offset] =
                    ((idx_num32_t *) series->index->idx)[i];
        }
    }
    if (offset)
    {
        series->index->len -= offset;
        series->index->idx = (idx_num32_t *) realloc(
                    (idx_num32_t *) series->index->idx,
                    series->index->len * sizeof(idx_num32_t));
        uint64_t start = shard->id - series->mask;
        uint64_t end = start + siridb->duration_num;
        if (series->start >= start && series->start < end)
        {
            SERIES_update_start_num32(series);
        }
        if (series->end < end && series->end > start)
        {
            SERIES_update_end_num32(series);
        }
    }
}

/*
 * Typedef: imap32_cb
 * Update series properties.
 */
int siridb_series_update_props(siridb_series_t * series, void * args)
{
    SERIES_update_start_num32(series);
    SERIES_update_end_num32(series);

    return 0;
}

siridb_points_t * siridb_series_get_points_num32(
        siridb_series_t * series,
        uint64_t * start_ts,
        uint64_t * end_ts)
{
    idx_num32_t * idx;
    siridb_points_t * points;
    siridb_point_t * point;
    size_t len, size;
    uint_fast32_t i;
    uint32_t indexes[series->index->len];

    len = i = size = 0;

    for (   idx = (idx_num32_t *) series->index->idx;
            i < series->index->len;
            i++, idx++)
    {
        if (    (start_ts == NULL || idx->end_ts >= *start_ts) &&
                (end_ts == NULL || idx->start_ts < *end_ts))
        {
            size += idx->len;
            indexes[len] = i;
            len++;
        }
    }

    size += series->buffer->points->len;
    points = siridb_points_new(size, series->tp);

    for (i = 0; i < len; i++)
    {
        siridb_shard_get_points_num32(
                points,
                (idx_num32_t *) series->index->idx + indexes[i],
                start_ts,
                end_ts,
                series->index->has_overlap);
        /* errors can be ignored here */
    }

    /* create pointer to buffer and get current length */
    point = series->buffer->points->data;
    len = series->buffer->points->len;

    /* crop start buffer if needed */
    if (start_ts != NULL)
    {
        for (; len && point->ts < *start_ts; point++, len--);
    }

    /* crop end buffer if needed */
    if (end_ts != NULL && len)
    {
        for (   siridb_point_t * p = point + len - 1;
                len && p->ts >= *end_ts;
                p--, len--);
    }

    /* add buffer points */
    for (; len; point++, len--)
    {
        siridb_points_add_point(points, &point->ts, &point->val);
    }

    if (points->len < size)
    {
        /* shrink allocation size */
        points->data = (siridb_point_t *)
                realloc(points->data, points->len * sizeof(siridb_point_t));
    }
#ifdef DEBUG
    else
    {
        /* size must be equal if not smaller */
        assert (points->len == size);
    }
#endif

    return points;
}

/*
 * Increment the series reference counter.
 */
inline void siridb_series_incref(siridb_series_t * series)
{
    series->ref++;
}

/*
 * Decrement reference counter for series and free the series when zero is
 * reached.
 */
void siridb_series_decref(siridb_series_t * series)
{
    if (!--series->ref)
    {
        SERIES_free(series);
    }
}

/*
 * Returns 0 if successful or -1 and a SIGNAL is raised in case of a critical
 * error.
 * Note that we also return 0 if we had to recover a shard. In this case you
 * can find the errors we had to recover in the log file. (log level should
 * be at least 'ERROR' for all error logs)
 */
int siridb_series_optimize_shard_num32(
        siridb_t * siridb,
        siridb_series_t * series,
        siridb_shard_t * shard)
{
#ifdef DEBUG
    assert (shard->id % siridb->duration_num == series->mask);
#endif

    idx_num32_t * idx;
    uint_fast32_t i, start, end, max_ts;
    size_t size;
    siridb_points_t * points;
    int rc = 0;

    max_ts = (shard->id + siridb->duration_num) - series->mask;

    end = i = size = 0;

    for (   idx = (idx_num32_t *) series->index->idx;
            i < series->index->len && idx->start_ts < max_ts;
            i++, idx++)
    {
        if (idx->shard == shard->replacing)
        {
            if (!end)
            {
                end = start = i;
            }
            size += idx->len;
            end++;
        } else if (idx->shard == shard && end)
        {
            end++;
        }
    }

    if (!end)
    {
        /* no data for this series is found in the shard */
        return rc;
    }

    long int pos;
    uint16_t chunk_sz;
    uint_fast32_t num_chunks, pstart, pend;

    points = siridb_points_new(size, series->tp);
    if (points == NULL)
    {
        return -1;  /* signal is raised */
    }

    for (i = start; i < end; i++)
    {
        idx = (idx_num32_t *) series->index->idx + i;
        /* we can have indexes for the 'new' shard which we should skip */
        if (idx->shard == shard->replacing && siridb_shard_get_points_num32(
                    points,
                    idx,
                    NULL,
                    NULL,
                    series->index->has_overlap))
        {
            /* an error occurred while reading points, logging is done */
            size -= idx->len;
        }
    }

    num_chunks = (size - 1) / siri.cfg->max_chunk_points + 1;
    chunk_sz = size / num_chunks + (size % num_chunks != 0);

    for (pstart = 0; pstart < size; pstart += chunk_sz)
    {
        pend = pstart + chunk_sz;
        if (pend > size)
        {
            pend = size;
        }

        if ((pos = siridb_shard_write_points(
                siridb,
                series,
                shard,
                points,
                pstart,
                pend)) == EOF)
        {
            log_critical("Cannot write points to shard id '%ld'", shard->id);
            rc = -1;  /* signal is raised */
        }
        else
        {
            idx = (idx_num32_t *) series->index->idx + start;
            idx->shard = shard;
            idx->start_ts = (uint32_t) points->data[pstart].ts;
            idx->end_ts = (uint32_t) points->data[pend - 1].ts;
            idx->len = pend - pstart;
            idx->pos = pos;
        }
        start++;
    }

    siridb_points_free(points);

    if (start < end)
    {
        /* save the difference in variable i */
        i = end - start;

        /* new length is current length minus difference */
        series->index->len -= i;

        for (; start < series->index->len; start++)
        {
            ((idx_num32_t *) series->index->idx)[start] =
                    ((idx_num32_t *) series->index->idx)[start + i];
        }

        /* shrink memory to the new size */
        idx = (idx_num32_t *) realloc(
                (idx_num32_t *) series->index->idx,
                series->index->len * sizeof(idx_num32_t));
        if (idx == NULL)
        {
            /* this is not critical since the original allocated block still
             * works.
             */
            log_error("Shrinking memory for one series has failed!");
        }
        else
        {
            series->index->idx = idx;
        }
    }
#ifdef DEBUG
    else
    {
        /* start must be equal to end if not smaller */
        assert (start == end);
    }
#endif
    return rc;
}

/*
 * Destroy series. (parsing NULL is not allowed)
 */
static void SERIES_free(siridb_series_t * series)
{
//    log_debug("Free series!");
    if (series->buffer != NULL)
    {
        siridb_buffer_free(series->buffer);
    }
    free(series->index->idx);
    free(series->index);
    free(series);
}

/*
 * Returns NULL and raises a SIGNAL in case an error has occurred.
 */
static siridb_series_t * SERIES_new(
        siridb_t * siridb,
        uint32_t id,
        uint8_t tp,
        const char * sn)
{
    uint32_t n = 0;
    siridb_series_t * series;
    series = (siridb_series_t *) malloc(sizeof(siridb_series_t));
    if (series == NULL)
    {
        ERR_ALLOC
    }
    else
    {
        series->id = id;
        series->tp = tp;
        series->ref = 1;
        series->length = 0;
        series->start = -1;
        series->end = 0;
        series->buffer = NULL;

        /* get sum series name to calculate series mask (for sharding) */
        for (; *sn; sn++)
        {
            n += *sn;
        }

        series->mask = (n / 11) % ((tp == SIRIDB_SERIES_TP_STRING) ?
                siridb->shard_mask_log : siridb->shard_mask_num);

        series->index =
                (siridb_series_idx_t *) malloc(sizeof(siridb_series_idx_t));
        if (series->index == NULL)
        {
            ERR_ALLOC
            free(series);
        }
        else
        {
            series->index->len = 0;
            series->index->has_overlap = 0;
            series->index->idx = NULL;
        }
    }
    return series;
}

/*
 * Raises a SIGNAL in case or an error.
 *
 * Returns always 0 but the result will be ignored since this function is used
 * in ct_walk().
 */
static int SERIES_pack(
        const char * key,
        siridb_series_t * series,
        qp_fpacker_t * fpacker)
{
    if (qp_fadd_type(fpacker, QP_ARRAY3) ||
        qp_fadd_raw(fpacker, key, strlen(key) + 1) ||
        qp_fadd_int32(fpacker, (int32_t) series->id) ||
        qp_fadd_int8(fpacker, (int8_t) series->tp))
    {
        ERR_FILE
    }
    return 0;  /* return code will be ignored */
}

/*
 * Returns 0 if successful or a negative integer in case of an error.
 * (SIGNAL is raised in case of an error)
 */
static int SERIES_save(siridb_t * siridb)
{
    qp_fpacker_t * fpacker;

    log_debug("Cleanup series file");

    /* macro get series file name */
    SIRIDB_GET_FN(fn, SIRIDB_SERIES_FN)

    if ((fpacker = qp_open(fn, "w")) == NULL)
    {
        ERR_FILE
        log_critical("Cannot open file '%s' for writing", fn);
        return EOF;
    }


    if (/* open a new array */
        qp_fadd_type(fpacker, QP_ARRAY_OPEN) ||

        /* write the current schema */
        qp_fadd_int16(fpacker, SIRIDB_SERIES_SCHEMA))
    {
        ERR_FILE
    }
    else
    {
        ct_walk(siridb->series, (ct_cb_t) &SERIES_pack, fpacker);
    }
    /* close file pointer */
    if (qp_close(fpacker))
    {
        ERR_FILE
    }

    return siri_err;
}

/*
 * Returns 0 if successful or -1 in case of an error.
 * (a SIGNAL might be raised but -1 should be considered critical in any case)
 */
static int SERIES_read_dropped(siridb_t * siridb, imap32_t * dropped)
{
    char * buffer;
    char * pt;
    long int size;
    int rc = 0;
    FILE * fp;

    log_debug("Read dropped series");

    SIRIDB_GET_FN(fn, SIRIDB_DROPPED_FN)

    if ((fp = fopen(fn, "r")) == NULL)
    {
        /* no drop file, we have nothing to do */
        return 0;
    }

    /* get file size */
    if (fseek(fp, 0, SEEK_END) ||
        (size = ftell(fp)) < 0 ||
        fseek(fp, 0, SEEK_SET))
    {
        fclose(fp);
        log_critical("Cannot read size of file '%s'", fn);
        rc = -1;
    }
    else if (size)
    {

        buffer = (char *) malloc(size);
        if (buffer == NULL)
        {
            log_critical("Cannot allocate buffer for reading dropped series");
            rc = -1;
        }
        else if (fread(buffer, size, 1, fp) == 1)
        {
            char * end = buffer + size;
            for (   pt = buffer;
                    pt < end;
                    pt += sizeof(uint32_t))
            {
                if (imap32_add(
                        dropped,
                        (uint32_t) *((uint32_t *) pt),
                        (int *) DROPPED_DUMMY))
                {
                    log_critical("Cannot add id to dropped map");
                    rc = -1;
                }
            }
        }
        else
        {
            log_critical("Cannot read %ld bytes from file '%s'", size, fn);
            rc = -1;
        }
        free(buffer);
    }

    fclose(fp);

    return rc;
}

static int SERIES_load(siridb_t * siridb, imap32_t * dropped)
{
    qp_unpacker_t * unpacker;
    qp_obj_t * qp_series_name;
    qp_obj_t * qp_series_id;
    qp_obj_t * qp_series_tp;
    siridb_series_t * series;
    qp_types_t tp;
    uint32_t series_id;

    /* we should not have any series at this moment */
    assert(siridb->max_series_id == 0);

    /* get series file name */
    SIRIDB_GET_FN(fn, SIRIDB_SERIES_FN)

    if (!xpath_file_exist(fn))
    {
        // missing series file, create an empty file and return
        return SERIES_save(siridb);
    }

    if ((unpacker = qp_unpacker_from_file(fn)) == NULL)
    {
        return -1;
    }

    /* unpacker will be freed in case schema check fails */
    siridb_schema_check(SIRIDB_SERIES_SCHEMA)

    qp_series_name = qp_object_new();
    qp_series_id = qp_object_new();
    qp_series_tp = qp_object_new();
    if (siri_err)
    {
        /* free objects */
        qp_object_free_safe(qp_series_name);
        qp_object_free_safe(qp_series_id);
        qp_object_free_safe(qp_series_tp);
        qp_unpacker_free(unpacker);
        return -1;  /* signal is raised */
    }

    while (qp_next(unpacker, NULL) == QP_ARRAY3 &&
            qp_next(unpacker, qp_series_name) == QP_RAW &&
            qp_next(unpacker, qp_series_id) == QP_INT64 &&
            qp_next(unpacker, qp_series_tp) == QP_INT64)
    {
        series_id = (uint32_t) qp_series_id->via->int64;

        /* update max_series_id */
        if (series_id > siridb->max_series_id)
        {
            siridb->max_series_id = series_id;
        }

        if (imap32_get(dropped, series_id) == NULL)
        {
            series = SERIES_new(
                    siridb,
                    series_id,
                    (uint8_t) qp_series_tp->via->int64,
                    qp_series_name->via->raw);
            if (series != NULL)
            {
                /* add series to c-tree */
                ct_add(siridb->series, qp_series_name->via->raw, series);

                /* add series to imap32 */
                imap32_add(siridb->series_map, series->id, series);
            }
        }
    }

    /* save last object, should be QP_END */
    tp = qp_next(unpacker, NULL);

    /* free objects */
    qp_object_free(qp_series_name);
    qp_object_free(qp_series_id);
    qp_object_free(qp_series_tp);

    /* free unpacker */
    qp_unpacker_free(unpacker);

    if (tp != QP_END)
    {
        log_critical("Expected end of file '%s'", fn);
        return -1;
    }

    /*
     * In case of a siri_err we should not overwrite series because the
     * file then might be incomplete.
     */
    if (siri_err || SERIES_save(siridb))
    {
        log_critical("Cannot write series index to disk");
        return -1;  /* signal is raised */
    }

    return 0;
}

/*
 * Open SiriDB drop series file.
 *
 * Returns 0 if successful or -1 in case of an error.
 */
static int SERIES_open_new_dropped_file(siridb_t * siridb)
{
    SIRIDB_GET_FN(fn, SIRIDB_DROPPED_FN)

    if ((siridb->dropped_fp = fopen(fn, "w")) == NULL)
    {
        log_critical("Cannot open '%s' for writing", fn);
        return -1;
    }
    return 0;
}

/*
 * Open SiriDB series store file.
 *
 * Returns 0 if successful or -1 in case of an error.
 */
static int SERIES_open_store(siridb_t * siridb)
{
    /* macro get series file name */
    SIRIDB_GET_FN(fn, SIRIDB_SERIES_FN)

    if ((siridb->store = qp_open(fn, "a")) == NULL)
    {
        log_critical("Cannot open file '%s' for appending", fn);
        return -1;
    }
    return 0;
}

/*
 * When series are dropped, the store still has this series so when
 * SiriDB starts the next time we will include this dropped series by
 * counting the max_series_id. A second restart could be a problem if
 * not all shards are optimized because now the store does not have the
 * last removed series and therefore the max_series_id could be set to
 * a value for which shards still have data. Creating a new series and
 * another SiriDB restart before the optimize has finished could lead
 * to problems.
 *
 * Saving max_series_id at startup solves this issue because it will
 * include the dropped series.
 *
 * Returns 0 if successful or -1 in case of an error.
 */
static int SERIES_update_max_id(siridb_t * siridb)
{
    int rc = 0;
    FILE * fp;
    uint32_t max_series_id = 0;

    SIRIDB_GET_FN(fn, SIRIDB_MAX_SERIES_ID_FN)

    if ((fp = fopen(fn, "r")) != NULL)
    {
        if (fread(&max_series_id, sizeof(uint32_t), 1, fp) != 1)
        {
            log_critical("Cannot read max_series_id from '%s'", fn);
            fclose(fp);
            return -1;
        }

        if (fclose(fp))
        {
            log_critical("Cannot close max_series_id file: '%s'", fn);
            return -1;
        }

        if (max_series_id > siridb->max_series_id)
        {
            siridb->max_series_id = max_series_id;
        }
    }

    /* we only need to write max_series_id in case the one in the file is
     * smaller or does not exist and max_series_id is larger than zero.
     */
    if (max_series_id < siridb->max_series_id)
    {
        if ((fp = fopen(fn, "w")) == NULL)
        {
            log_critical("Cannot open file '%s' for writing", fn);
            return -1;
        }

        log_debug("Write max series id (%ld)", siridb->max_series_id);

        if (fwrite(&siridb->max_series_id, sizeof(uint32_t), 1, fp) != 1)
        {
            log_critical("Cannot write max_series_id to file '%s'", fn);
            rc = -1;
        }

        if (fclose(fp))
        {
            log_critical("Cannot save max_series_id to file '%s'", fn);
            rc = -1;
        }
    }
    return rc;
}

/*
 * Update series 'start' property.
 * (integer/float series with 32bit time-stamps)
 */
static void SERIES_update_start_num32(siridb_series_t * series)
{
    series->start = (series->index->len) ?
            ((idx_num32_t *) series->index->idx)->start_ts : -1;

    if (series->buffer->points->len)
    {
        siridb_point_t * point = series->buffer->points->data;
        if (point->ts < series->start)
        {
            series->start = point->ts;
        }
    }
}

/*
 * Update series 'end' property.
 * (integer/float series with 32bit time-stamps)
 */
static void SERIES_update_end_num32(siridb_series_t * series)
{
    if (series->index->len)
    {
        uint32_t start = 0;
        idx_num32_t * idx;
        for (uint_fast32_t i = series->index->len; i--;)
        {
            idx = (idx_num32_t *) series->index->idx + i;

            if (idx->end_ts < start)
            {
                break;
            }

            start = idx->start_ts;
            if (idx->end_ts > series->end)
            {
                series->end = idx->end_ts;
            }
        }
    }
    else
    {
        series->end = 0;
    }
    if (series->buffer->points->len)
    {
        siridb_point_t * point = series->buffer->points->data +
                series->buffer->points->len - 1;
        if (point->ts > series->end)
        {
            series->end = point->ts;
        }
    }
}


