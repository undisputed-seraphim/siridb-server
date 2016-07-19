/*
 * pool.c - Generate pool lookup.
 *
 * author       : Jeroen van der Heijden
 * email        : jeroen@transceptor.technology
 * copyright    : 2016, Transceptor Technology
 *
 * changes
 *  - initial version, 25-03-2016
 *
 */

#include <siri/db/pool.h>
#include <siri/db/pools.h>
#include <stdlib.h>
#include <string.h>
#include <logger/logger.h>
#include <siri/grammar/grammar.h>
#include <assert.h>


/*
 * Returns a pool id based on a terminated string.
 */
uint16_t siridb_pool_sn(
        siridb_t * siridb,
        const char * sn)
{
    uint32_t n = 0;
    for (; *sn; sn++)
    {
        n += *sn;
    }
    return (*siridb->pools->lookup)[n % SIRIDB_LOOKUP_SZ];
}

/*
 * Returns a pool id based on a raw string.
 */
uint16_t siridb_pool_sn_raw(
        siridb_t * siridb,
        const char * sn,
        size_t len)
{
    uint32_t n = 0;
    while (len--)
    {
        n += sn[len];
    }
    return (*siridb->pools->lookup)[n % SIRIDB_LOOKUP_SZ];
}

/*
 * Returns 1 (true) if at least one server in the pool is online, 0 (false)
 * if no server in the pool is online.
 *
 * Warning: this function should not be used on 'this' pool.
 *
 * A server is  'online' when at least connected and authenticated.
 */
int siridb_pool_online(siridb_pool_t * pool)
{
    for (uint16_t i = 0; i < pool->len; i++)
    {
        if (siridb_server_is_online(pool->server[i]))
        {
            return 1;  // true
        }
    }
    return 0;  // false
}

/*
 * Returns 1 (true) if at least one server in the pool is available, 0 (false)
 * if no server in the pool is available.
 *
 * Warning: this function should not be used on 'this' pool.
 *
 * A server is  'available' when and ONLY when connected and authenticated.
 */
int siridb_pool_available(siridb_pool_t * pool)
{
    for (uint16_t i = 0; i < pool->len; i++)
    {
        if (siridb_server_is_available(pool->server[i]))
        {
            return 1;  // true
        }
    }
    return 0;  // false
}

/*
 * Call-back function used to validate pools in a where expression.
 *
 * Returns 0 or 1 (false or true).
 */
int siridb_pool_cexpr_cb(siridb_pool_walker_t * wpool, cexpr_condition_t * cond)
{
    switch (cond->prop)
    {
    case CLERI_GID_K_POOL:
        return cexpr_int_cmp(cond->operator, wpool->pid, cond->int64);
    case CLERI_GID_K_SERVERS:
        return cexpr_int_cmp(cond->operator, wpool->servers, cond->int64);
    case CLERI_GID_K_SERIES:
        return cexpr_int_cmp(cond->operator, wpool->series, cond->int64);
    }
    /* we must NEVER get here */
    log_critical("Unexpected pool property received: %d", cond->prop);
    assert (0);
    return -1;
}

/*
 * Returns 0 if we have send the package to one available
 * server in the pool. The package will be send only to one server, even if
 * the pool has more servers available.
 *
 * This function can raise a SIGNAL when allocation errors occur but be aware
 * that 0 can still be returned in this case.
 *
 * The call-back function is not called when -1 is returned.
 *
 * A server is  'available' when and ONLY when connected and authenticated.
 *
 * Note that 'pkg->pid' will be overwritten with a new package id.
 */
int siridb_pool_send_pkg(
        siridb_pool_t * pool,
        sirinet_pkg_t * pkg,
        uint64_t timeout,
        sirinet_promise_cb cb,
        void * data)
{
    siridb_server_t * server = NULL;

    for (uint16_t i = 0; i < pool->len; i++)
    {
        if (siridb_server_is_available(pool->server[i]))
        {
            server = (server == NULL) ?
                    pool->server[i] : pool->server[rand() % 2];
        }
    }

    if (server == NULL)
    {
        return -1;
    }
    else
    {
        siridb_server_send_pkg(server, pkg, timeout, cb, data);
    }
    return 0;
}
