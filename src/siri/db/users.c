/*
 * users.c - contains functions for a SiriDB database members.
 *
 * author       : Jeroen van der Heijden
 * email        : jeroen@transceptor.technology
 * copyright    : 2016, Transceptor Technology
 *
 * changes
 *  - initial version, 04-05-2016
 *
 */

#include <siri/db/users.h>
#include <siri/db/query.h>
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <crypt.h>
#include <time.h>
#include <xpath/xpath.h>
#include <qpack/qpack.h>
#include <strextra/strextra.h>
#include <logger/logger.h>

#define SIRIDB_MIN_USER_LEN 2
#define SIRIDB_MAX_USER_LEN 60

#define SIRIDB_USERS_SCHEMA 1
#define SIRIDB_USERS_FN "users.dat"

inline static int USERS_cmp(siridb_user_t * user, const char * name);
static int USERS_free(siridb_user_t * user, void * args);
static int USERS_save(siridb_user_t * user, qp_fpacker_t * fpacker);

/*
 * Returns 0 if successful or -1 in case of an error.
 * (a SIGNAL might be raised in case of an error)
 */
int siridb_users_load(siridb_t * siridb)
{
    qp_unpacker_t * unpacker;
    qp_obj_t * username;
    qp_obj_t * password;
    qp_obj_t * access_bit;
    siridb_user_t * user;
    char err_msg[SIRIDB_MAX_SIZE_ERR_MSG];

    /* we should not have any users at this moment */
    assert(siridb->users == NULL);

    /* create a new user list */
    siridb->users = llist_new();
    if (siridb->users == NULL)
    {
        return -1;  /* signal is raised */
    }

    /* get user access file name */
    SIRIDB_GET_FN(fn, SIRIDB_USERS_FN)

    if (!xpath_file_exist(fn))
    {
        /* we do not have a user access file, lets create the first user */
        user = siridb_user_new();
        if (user == NULL)
        {
            return -1;  /* signal is raised */
        }
        siridb_user_incref(user);
        user->username = strdup("iris");
        if (user->username == NULL)
        {
            ERR_ALLOC
            siridb_user_decref(user);
            return -1;
        }
        user->access_bit = SIRIDB_ACCESS_PROFILE_FULL;

        if (    siridb_user_set_password(user, "siri", err_msg) ||
                siridb_users_add_user(siridb, user, err_msg))
        {
            log_error("%s", err_msg);
            siridb_user_decref(user);
            return -1;
        }

        return 0;
    }

    if ((unpacker = qp_unpacker_from_file(fn)) == NULL)
    {
        return -1;  /* a signal is raised is case of a memory error */
    }

    /* unpacker will be freed in case macro fails */
    siridb_schema_check(SIRIDB_USERS_SCHEMA)

    username = qp_object_new();
    password = qp_object_new();
    access_bit = qp_object_new();

    if (username == NULL || password == NULL || access_bit == NULL)
    {
        qp_object_free_safe(username);
        qp_object_free_safe(password);
        qp_object_free_safe(access_bit);
        qp_unpacker_free(unpacker);
        return -1;  /* signal is raised */
    }

    int rc = 0;
    while (qp_is_array(qp_next(unpacker, NULL)) &&
            qp_next(unpacker, username) == QP_RAW &&
            qp_next(unpacker, password) == QP_RAW &&
            qp_next(unpacker, access_bit) == QP_INT64)
    {
        user = siridb_user_new();
        if (user == NULL)
        {
            rc = -1;  /* signal is raised */
        }
        else
        {
            siridb_user_incref(user);

            user->username = strndup(username->via->raw, username->len);
            user->password = strndup(password->via->raw, password->len);

            if (user->username == NULL || user->password == NULL)
            {
                ERR_ALLOC
                siridb_user_decref(user);
                rc = -1;
            }
            else
            {
                user->access_bit = (uint32_t) access_bit->via->int64;
                if (llist_append(siridb->users, user))
                {
                    siridb_user_decref(user);
                    rc = -1;  /* signal is raised */
                }
            }
        }
    }

    /* free objects */
    qp_object_free(username);
    qp_object_free(password);
    qp_object_free(access_bit);

    /* free unpacker */
    qp_unpacker_free(unpacker);

    return rc;
}

/*
 * Typedef: sirinet_clserver_get_file
 *
 * Returns the length of the content for a file and set buffer with the file
 * content. Note that malloc is used to allocate memory for the buffer.
 *
 * In case of an error -1 is returned and buffer will be set to NULL.
 */
ssize_t siridb_users_get_file(char ** buffer, siridb_t * siridb)
{
    /* get users file name */
    SIRIDB_GET_FN(fn, SIRIDB_USERS_FN)

    return xpath_get_content(buffer, fn);
}

/*
 * Destroy servers, parsing NULL is not allowed.
 */
void siridb_users_free(llist_t * users)
{
    llist_free_cb(users, (llist_cb) USERS_free, NULL);
}

/*
 * Returns 0 when successful, a value greater then zero for expected errors
 * like invalid user, password etc, and -1 is returned in case of a critical
 * error. (a critical error also raises a signal). The err_msg will cantain
 * the error in any case.
 */
int siridb_users_add_user(
        siridb_t * siridb,
        siridb_user_t * user,
        char * err_msg)
{
    if (strlen(user->username) < SIRIDB_MIN_USER_LEN)
    {
        sprintf(err_msg, "User name should be at least %d characters.",
                SIRIDB_MIN_USER_LEN);
        return 1;
    }

    if (strlen(user->username) > SIRIDB_MAX_USER_LEN)
    {
        sprintf(err_msg, "User name should be at least %d characters.",
                SIRIDB_MAX_USER_LEN);
        return 1;
    }

    if (!strx_is_graph(user->username))
    {
        sprintf(err_msg,
                "User name contains illegal characters. (only graphical "
                "characters are allowed, no spaces, tabs etc.)");
        return 1;
    }

    if (llist_get(siridb->users, (llist_cb) USERS_cmp, user->username) != NULL)
    {
        snprintf(err_msg,
                SIRIDB_MAX_SIZE_ERR_MSG,
                "User name '%s' already exists.",
                user->username);
        return 1;
    }

    /* add the user to the users */
    if (llist_append(siridb->users, user))
    {
        /* this is critical, a signal is raises */
        return -1;
    }

    if (siridb_users_save(siridb))
    {
        /* this is critical, a signal is raises */
        snprintf(err_msg,
                SIRIDB_MAX_SIZE_ERR_MSG,
                "Could not save user '%s' to file.",
                user->username);
        log_critical(err_msg);
        return -1;
    }

    return 0;
}

/*
 * Returns NULL when the user is not found of when the given password is
 * incorrect. When *password is NULL the password will NOT be checked and
 * the user will be returned when found.
 */
siridb_user_t * siridb_users_get_user(
        llist_t * users,
        const char * username,
        const char * password)
{
    siridb_user_t * user;
    char * pw;

    if ((user = llist_get(
            users,
            (llist_cb) USERS_cmp,
            (void *) username)) == NULL)
    {
        return NULL;
    }

    if (password == NULL)
    {
        return user;
    }

    pw = crypt(password, user->password);

    return (strcmp(pw, user->password) == 0) ? user : NULL;
}

int siridb_users_drop_user(
        siridb_t * siridb,
        const char * username,
        char * err_msg)
{
    siridb_user_t * user;

    if ((user = llist_remove(
            siridb->users,
            (llist_cb) USERS_cmp,
            (void *) username)) == NULL)
    {
        snprintf(err_msg,
                SIRIDB_MAX_SIZE_ERR_MSG,
                "User '%s' does not exist.",
                username);
        return 1;
    }

    /* decrement reference for user object */
    siridb_user_decref(user);

    if (siridb_users_save(siridb))
    {
        log_critical("Could not write users to file!");
    }

    return 0;
}

/*
 * Returns 0 if successful; EOF and a signal is raised in case an error occurred.
 */
int siridb_users_save(siridb_t * siridb)
{
    qp_fpacker_t * fpacker;

    /* get user access file name */
    SIRIDB_GET_FN(fn, SIRIDB_USERS_FN)

    if (
        /* open a new user file */
        (fpacker = qp_open(fn, "w")) == NULL ||

        /* open a new array */
        qp_fadd_type(fpacker, QP_ARRAY_OPEN) ||

        /* write the current schema */
        qp_fadd_int16(fpacker, SIRIDB_USERS_SCHEMA) ||

        /* we can and should skip this if we have no users to save */
        llist_walk(siridb->users, (llist_cb) USERS_save, fpacker) ||

        /* close file pointer */
        qp_close(fpacker))
    {
        ERR_FILE
        return EOF;
    }

    return 0;
}

/*
 * Returns 0 if successful and -1 in case an error occurred.
 */
static int USERS_save(siridb_user_t * user, qp_fpacker_t * fpacker)
{
    int rc = 0;

    rc += qp_fadd_type(fpacker, QP_ARRAY3);
    rc += qp_fadd_string(fpacker, user->username);
    rc += qp_fadd_string(fpacker, user->password);
    rc += qp_fadd_int32(fpacker, (int32_t) user->access_bit);
    return rc;
}

inline static int USERS_cmp(siridb_user_t * user, const char * name)
{
    return (strcmp(user->username, name) == 0);
}

static int USERS_free(siridb_user_t * user, void * args)
{
    siridb_user_decref(user);
    return 0;
}

