/* -*- Mode: C; tab-width: 4 -*- */
/*
  +----------------------------------------------------------------------+
  | PHP Version 5                                                        |
  +----------------------------------------------------------------------+
  | Copyright (c) 1997-2009 The PHP Group                                |
  +----------------------------------------------------------------------+
  | This source file is subject to version 3.01 of the PHP license,      |
  | that is bundled with this package in the file LICENSE, and is        |
  | available through the world-wide-web at the following url:           |
  | http://www.php.net/license/3_01.txt                                  |
  | If you did not receive a copy of the PHP license and are unable to   |
  | obtain it through the world-wide-web, please send a note to          |
  | license@php.net so we can mail you a copy immediately.               |
  +----------------------------------------------------------------------+
  | Original author: Alfonso Jimenez <yo@alfonsojimenez.com>             |
  | Maintainer: Nicolas Favre-Felix <n.favre-felix@owlient.eu>           |
  | Maintainer: Nasreddine Bouafif <n.bouafif@owlient.eu>                |
  +----------------------------------------------------------------------+
*/

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#ifdef PHP_SESSION
#include "common.h"
#include "ext/standard/info.h"
#include "php_redis.h"
#include "redis_session.h"
#include <zend_exceptions.h>

#include "library.h"

#include "php.h"
#include "php_ini.h"
#include "php_variables.h"
#include "SAPI.h"
#include "ext/standard/url.h"

ps_module ps_mod_redis = {
	PS_MOD(redis)
};

typedef struct redis_pool_member_ {

	RedisSock *redis_sock;
	int weight;

    char *prefix;
    size_t prefix_len;

    char *auth;
    size_t auth_len;

    RedisSock *failover_sock;

	struct redis_pool_member_ *next;

} redis_pool_member;

typedef struct {

	int totalWeight;
	int count;

	redis_pool_member *head;

} redis_pool;

PHPAPI redis_pool*
redis_pool_new(TSRMLS_D) {
	return ecalloc(1, sizeof(redis_pool));
}

PHPAPI void
redis_pool_add(redis_pool *pool, RedisSock *redis_sock, int weight,
                char *prefix, char *auth, RedisSock *failover_sock TSRMLS_DC) {

	redis_pool_member *rpm = ecalloc(1, sizeof(redis_pool_member));
	rpm->redis_sock = redis_sock;
	rpm->weight = weight;

    rpm->prefix = prefix;
    rpm->prefix_len = (prefix?strlen(prefix):0);

    rpm->auth = auth;
    rpm->auth_len = (auth?strlen(auth):0);

	rpm->failover_sock = failover_sock;

	rpm->next = pool->head;
	pool->head = rpm;

	pool->totalWeight += weight;
}

PHPAPI void
redis_pool_free(redis_pool *pool TSRMLS_DC) {

	redis_pool_member *rpm, *next;
    rpm = pool->head;
	while(rpm) {
		next = rpm->next;
		redis_sock_disconnect(rpm->redis_sock TSRMLS_CC);
		efree(rpm->redis_sock);
		if(rpm->prefix) efree(rpm->prefix);
		if(rpm->auth) efree(rpm->auth);
		efree(rpm);
		rpm = next;
	}
	efree(pool);
}

static void
redis_auth(RedisSock *sock, char *cmd, int cmd_len TSRMLS_DC) {

    char *response;
    int response_len;

    if(redis_sock_write(sock, cmd, cmd_len TSRMLS_CC) >= 0) {
        if ((response = redis_sock_read(sock, &response_len TSRMLS_CC))) {
            efree(response);
        }
    }
}

void
redis_pool_member_auth(redis_pool_member *rpm TSRMLS_DC) {
    char *cmd;
    int cmd_len;

    if(!rpm->auth || !rpm->auth_len) { /* no password given. */
            return;
    }
    cmd_len = redis_cmd_format_static(&cmd, "AUTH", "s", rpm->auth, rpm->auth_len);

    redis_auth(rpm->redis_sock, cmd, cmd_len TSRMLS_CC);
    if(rpm->failover_sock)
        redis_auth(rpm->failover_sock, cmd, cmd_len TSRMLS_CC);
    efree(cmd);
}

PHPAPI redis_pool_member *
redis_pool_get_sock(redis_pool *pool, const char *key TSRMLS_DC) {

	unsigned int pos, i;
	memcpy(&pos, key, sizeof(pos));
	pos %= pool->totalWeight;

	redis_pool_member *rpm = pool->head;

	for(i = 0; i < pool->totalWeight;) {
		if(pos >= i && pos < i + rpm->weight) {
			int needs_auth = 0;
            if(rpm->auth && rpm->auth_len && rpm->redis_sock->status != REDIS_SOCK_STATUS_CONNECTED) {
                    needs_auth = 1;
            }
            redis_sock_server_open(rpm->redis_sock, 0 TSRMLS_CC);
            if(rpm->failover_sock)
                redis_sock_server_open(rpm->failover_sock, 0 TSRMLS_CC);

            if(needs_auth) {
                redis_pool_member_auth(rpm TSRMLS_CC);
            }

			return rpm;
		}
		i += rpm->weight;
        rpm = rpm->next;
	}

	return NULL;
}

/* {{{ PS_OPEN_FUNC
 */
PS_OPEN_FUNC(redis)
{

	php_url *url;
	zval *params, **param;
	int i, j, path_len;

	redis_pool *pool = redis_pool_new(TSRMLS_C);

	for (i=0,j=0,path_len=strlen(save_path); i<path_len; i=j+1) {
		/* find beginning of url */
		while (i<path_len && (isspace(save_path[i]) || save_path[i] == ','))
			i++;

		/* find end of url */
		j = i;
		while (j<path_len && !isspace(save_path[j]) && save_path[j] != ',')
			 j++;

		if (i < j) {
			int weight = 1;
			double timeout = 86400.0;
			int persistent = 0;
            char *prefix = NULL, *auth = NULL, *persistent_id = NULL, *failover = NULL;

            /* translate unix: into file: */
			if (!strncmp(save_path+i, "unix:", sizeof("unix:")-1)) {
				int len = j-i;
				char *path = estrndup(save_path+i, len);
				memcpy(path, "file:", sizeof("file:")-1);
				url = php_url_parse_ex(path, len);
				efree(path);
			} else {
				url = php_url_parse_ex(save_path+i, j-i);
			}

			if (!url) {
				char *path = estrndup(save_path+i, j-i);
				php_error_docref(NULL TSRMLS_CC, E_WARNING,
					"Failed to parse session.save_path (error at offset %d, url was '%s')", i, path);
				efree(path);

				redis_pool_free(pool TSRMLS_CC);
				PS_SET_MOD_DATA(NULL);
				return FAILURE;
			}

			/* parse parameters */
			if (url->query != NULL) {
				MAKE_STD_ZVAL(params);
				array_init(params);

				sapi_module.treat_data(PARSE_STRING, estrdup(url->query), params TSRMLS_CC);

				if (zend_hash_find(Z_ARRVAL_P(params), "weight", sizeof("weight"), (void **) &param) != FAILURE) {
					convert_to_long_ex(param);
					weight = Z_LVAL_PP(param);
				}

				if (zend_hash_find(Z_ARRVAL_P(params), "timeout", sizeof("timeout"), (void **) &param) != FAILURE) {
					timeout = atof(Z_STRVAL_PP(param));
				}
				if (zend_hash_find(Z_ARRVAL_P(params), "persistent", sizeof("persistent"), (void **) &param) != FAILURE) {
					persistent = (atol(Z_STRVAL_PP(param)) == 1 ? 1 : 0);
				}
				if (zend_hash_find(Z_ARRVAL_P(params), "persistent_id", sizeof("persistent_id"), (void **) &param) != FAILURE) {
					persistent_id = estrndup(Z_STRVAL_PP(param), Z_STRLEN_PP(param));
				}
				if (zend_hash_find(Z_ARRVAL_P(params), "prefix", sizeof("prefix"), (void **) &param) != FAILURE) {
					prefix = estrndup(Z_STRVAL_PP(param), Z_STRLEN_PP(param));
				}
				if (zend_hash_find(Z_ARRVAL_P(params), "auth", sizeof("auth"), (void **) &param) != FAILURE) {
					auth = estrndup(Z_STRVAL_PP(param), Z_STRLEN_PP(param));
				}
				if (zend_hash_find(Z_ARRVAL_P(params), "failover", sizeof("failover"), (void **) &param) != FAILURE) {
					failover = estrndup(Z_STRVAL_PP(param), Z_STRLEN_PP(param));
				}

				/* // not supported yet
				if (zend_hash_find(Z_ARRVAL_P(params), "retry_interval", sizeof("retry_interval"), (void **) &param) != FAILURE) {
					convert_to_long_ex(param);
					retry_interval = Z_LVAL_PP(param);
				}
				*/

				zval_ptr_dtor(&params);
			}

			if ((url->path == NULL && url->host == NULL) || weight <= 0 || timeout <= 0) {
				php_url_free(url);
				redis_pool_free(pool TSRMLS_CC);
				PS_SET_MOD_DATA(NULL);
				return FAILURE;
			}

			RedisSock *redis_sock, *failover_sock = NULL;
            if(url->path) { /* unix */
                    redis_sock = redis_sock_create(url->path, strlen(url->path), 0, timeout, persistent, persistent_id);
            } else {
                    redis_sock = redis_sock_create(url->host, strlen(url->host), url->port, timeout, persistent, persistent_id);
            }

            if(failover) {
                short port = 6379;
                char *p = strchr(failover, ':');
                size_t sz = strlen(failover);
                if(p) {
                    port = atoi(p+1);
                    sz = p - failover;
                }
                failover_sock = redis_sock_create(failover, sz, port, timeout, persistent, persistent_id);
            }

			redis_pool_add(pool, redis_sock, weight, prefix, auth, failover_sock TSRMLS_CC);

			php_url_free(url);
		}
	}

	if (pool->head) {
		PS_SET_MOD_DATA(pool);
		return SUCCESS;
	}

	return FAILURE;
}
/* }}} */

/* {{{ PS_CLOSE_FUNC
 */
PS_CLOSE_FUNC(redis)
{
	redis_pool *pool = PS_GET_MOD_DATA();

	if(pool){
		redis_pool_free(pool TSRMLS_CC);
		PS_SET_MOD_DATA(NULL);
	}
	return SUCCESS;
}
/* }}} */

static char *
redis_session_key(redis_pool_member *rpm, const char *key, int key_len, int *session_len) {

	char *session;
    char default_prefix[] = "PHPREDIS_SESSION:";
    char *prefix = default_prefix;
    size_t prefix_len = sizeof(default_prefix)-1;

    if(rpm->prefix) {
        prefix = rpm->prefix;
        prefix_len = rpm->prefix_len;
    }

	/* build session key */
	*session_len = key_len + prefix_len;
	session = emalloc(*session_len);
    memcpy(session, prefix, prefix_len);
	memcpy(session + prefix_len, key, key_len);

	return session;
}

static int
get_session(redis_pool_member *rpm, RedisSock *sock, const char *key, char **val, int *vallen TSRMLS_DC)
{
	char *session, *cmd;
	int session_len, cmd_len, ret;

    /* send GET command */
	session = redis_session_key(rpm, key, strlen(key), &session_len);
	cmd_len = redis_cmd_format_static(&cmd, "GET", "s", session, session_len);
	efree(session);
	if(redis_sock_write(sock, cmd, cmd_len TSRMLS_CC) < 0) {
		efree(cmd);
		return FAILURE;
	}
	efree(cmd);

	/* read response */
	if ((*val = redis_sock_read(sock, vallen TSRMLS_CC)) == NULL) {
		return FAILURE;
	}

	return SUCCESS;
}

/* {{{ PS_READ_FUNC
 */
PS_READ_FUNC(redis)
{
	int ret;

	redis_pool *pool = PS_GET_MOD_DATA();
    redis_pool_member *rpm = redis_pool_get_sock(pool, key TSRMLS_CC);
	RedisSock *redis_sock = rpm?rpm->redis_sock:NULL, *failover_sock;
	if(!rpm || !redis_sock){
		return FAILURE;
	}

    /* read session from main server or failover. */
    ret = get_session(rpm, redis_sock, key, val, vallen TSRMLS_CC);
    if(EG(exception) && rpm->failover_sock) {
		zend_clear_exception(TSRMLS_C); /* clear exception flag. */
        ret = get_session(rpm, rpm->failover_sock, key, val, vallen TSRMLS_CC);
    }

    return ret;
}
/* }}} */

static int
set_session(redis_pool_member *rpm, RedisSock *sock, const char *session, int session_len, const char *val, int vallen TSRMLS_DC)
{
	char *cmd, *response;
	int cmd_len, response_len;

    /* send SET command */
	cmd_len = redis_cmd_format_static(&cmd, "SETEX", "sds", session, session_len, INI_INT("session.gc_maxlifetime"), val, vallen);
	if(redis_sock_write(sock, cmd, cmd_len TSRMLS_CC) < 0) {
		efree(cmd);
		return FAILURE;
	}
	efree(cmd);

	/* read response */
	if ((response = redis_sock_read(sock, &response_len TSRMLS_CC)) == NULL) {
		return FAILURE;
	}

	if(response_len == 3 && strncmp(response, "+OK", 3) == 0) {
		efree(response);
		return SUCCESS;
	} else {
		efree(response);
		return FAILURE;
	}
}

/* {{{ PS_WRITE_FUNC
 */
PS_WRITE_FUNC(redis)
{
    int ret;
    char *session;
    int session_len;

	redis_pool *pool = PS_GET_MOD_DATA();
    redis_pool_member *rpm = redis_pool_get_sock(pool, key TSRMLS_CC);
	RedisSock *redis_sock = rpm?rpm->redis_sock:NULL;
	if(!rpm || !redis_sock){
		return FAILURE;
	}

    /* disable exceptions */
    redis_sock->nothrow = 1;

    /* write session to main server or failover. */
	session = redis_session_key(rpm, key, strlen(key), &session_len);
    ret = set_session(rpm, redis_sock, session, session_len, val, vallen TSRMLS_CC);
    if(ret == FAILURE && rpm->failover_sock) {
        ret = set_session(rpm, rpm->failover_sock, session, session_len, val, vallen TSRMLS_CC);
    }
	efree(session);

    /* re-enable exceptions */
    redis_sock->nothrow = 0;

    return ret;
}
/* }}} */

/* {{{ PS_DESTROY_FUNC
 */
PS_DESTROY_FUNC(redis)
{
	char *cmd, *response, *session;
	int cmd_len, response_len, session_len;

	redis_pool *pool = PS_GET_MOD_DATA();
    redis_pool_member *rpm = redis_pool_get_sock(pool, key TSRMLS_CC);
	RedisSock *redis_sock = rpm?rpm->redis_sock:NULL;
	if(!rpm || !redis_sock){
		return FAILURE;
	}

    /* send DEL command */
	session = redis_session_key(rpm, key, strlen(key), &session_len);
	cmd_len = redis_cmd_format_static(&cmd, "DEL", "s", session, session_len);
	efree(session);
	if(redis_sock_write(redis_sock, cmd, cmd_len TSRMLS_CC) < 0) {
		efree(cmd);
		return FAILURE;
	}
	efree(cmd);

	/* read response */
	if ((response = redis_sock_read(redis_sock, &response_len TSRMLS_CC)) == NULL) {
		return FAILURE;
	}

	if(response_len == 2 && response[0] == ':' && (response[1] == '0' || response[1] == '1')) {
		efree(response);
		return SUCCESS;
	} else {
		efree(response);
		return FAILURE;
	}
}
/* }}} */

/* {{{ PS_GC_FUNC
 */
PS_GC_FUNC(redis)
{
	return SUCCESS;
}
/* }}} */

#endif
/* vim: set tabstop=4 expandtab: */

