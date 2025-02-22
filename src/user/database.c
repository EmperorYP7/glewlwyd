/**
 *
 * Glewlwyd SSO Server
 *
 * Authentiation server
 * Users are authenticated via various backend available: database, ldap
 * Using various authentication methods available: password, OTP, send code, etc.
 * 
 * Database user module
 * 
 * Copyright 2016-2020 Nicolas Mora <mail@babelouest.org>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU GENERAL PUBLIC LICENSE
 * License as published by the Free Software Foundation;
 * version 3 of the License.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU GENERAL PUBLIC LICENSE for more details.
 *
 * You should have received a copy of the GNU General Public
 * License along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include <string.h>
#include <jansson.h>
#include <yder.h>
#include <orcania.h>
#include <hoel.h>
#include "glewlwyd-common.h"

#define G_TABLE_USER "g_user"
#define G_TABLE_USER_SCOPE "g_user_scope"
#define G_TABLE_USER_SCOPE_USER "g_user_scope_user"
#define G_TABLE_USER_PROPERTY "g_user_property"
#define G_TABLE_USER_PASSWORD "g_user_password"

#define G_PBKDF2_ITERATOR_SEP ','

struct mod_parameters {
  int use_glewlwyd_connection;
  digest_algorithm hash_algorithm;
  struct _h_connection * conn;
  json_t * j_params;
  int multiple_passwords;
  unsigned int PBKDF2_iterations;
};

static json_t * is_user_database_parameters_valid(json_t * j_params) {
  json_t * j_return, * j_error = json_array(), * j_element = NULL;
  const char * field = NULL;
  
  if (j_error != NULL) {
    if (!json_is_object(j_params)) {
      json_array_append_new(j_error, json_string("parameters must be a JSON object"));
    } else {
      if (json_object_get(j_params, "use-glewlwyd-connection") != NULL && !json_is_boolean(json_object_get(j_params, "use-glewlwyd-connection"))) {
        json_array_append_new(j_error, json_string("use-glewlwyd-connection must be a boolean"));
      }
      if (json_object_get(j_params, "use-glewlwyd-connection") == json_false()) {
        if (json_object_get(j_params, "connection-type") == NULL || !json_is_string(json_object_get(j_params, "connection-type")) || (0 != o_strcmp("sqlite", json_string_value(json_object_get(j_params, "connection-type"))) && 0 != o_strcmp("mariadb", json_string_value(json_object_get(j_params, "connection-type"))) && 0 != o_strcmp("postgre", json_string_value(json_object_get(j_params, "connection-type"))))) {
          json_array_append_new(j_error, json_string("connection-type is mandatory and must be one of the following values: 'sqlite', 'mariadb', 'postgre'"));
        } else if (0 == o_strcmp("sqlite", json_string_value(json_object_get(j_params, "connection-type")))) {
          if (json_object_get(j_params, "sqlite-dbpath") == NULL || !json_is_string(json_object_get(j_params, "sqlite-dbpath"))) {
            json_array_append_new(j_error, json_string("sqlite-dbpath is mandatory and must be a string"));
          }
        } else if (0 == o_strcmp("mariadb", json_string_value(json_object_get(j_params, "connection-type")))) {
          if (json_object_get(j_params, "mariadb-host") == NULL || !json_is_string(json_object_get(j_params, "mariadb-host"))) {
            json_array_append_new(j_error, json_string("mariadb-host is mandatory and must be a string"));
          }
          if (json_object_get(j_params, "mariadb-user") == NULL || !json_is_string(json_object_get(j_params, "mariadb-user"))) {
            json_array_append_new(j_error, json_string("mariadb-user is mandatory and must be a string"));
          }
          if (json_object_get(j_params, "mariadb-password") == NULL || !json_is_string(json_object_get(j_params, "mariadb-password"))) {
            json_array_append_new(j_error, json_string("mariadb-password is mandatory and must be a string"));
          }
          if (json_object_get(j_params, "mariadb-dbname") == NULL || !json_is_string(json_object_get(j_params, "mariadb-dbname"))) {
            json_array_append_new(j_error, json_string("mariadb-dbname is mandatory and must be a string"));
          }
          if (json_object_get(j_params, "mariadb-port") != NULL && (!json_is_integer(json_object_get(j_params, "mariadb-dbname")) || json_integer_value(json_object_get(j_params, "mariadb-dbname")) < 0)) {
            json_array_append_new(j_error, json_string("mariadb-port is optional and must be a positive integer (default: 0)"));
          }
        } else if (0 == o_strcmp("postgre", json_string_value(json_object_get(j_params, "connection-type")))) {
          if (json_object_get(j_params, "postgre-conninfo") == NULL || !json_is_string(json_object_get(j_params, "postgre-conninfo"))) {
            json_array_append_new(j_error, json_string("postgre-conninfo is mandatory and must be a string"));
          }
        }
      }
      if (json_object_get(j_params, "data-format") != NULL) {
        if (!json_is_object(json_object_get(j_params, "data-format"))) {
          json_array_append_new(j_error, json_string("data-format is optional and must be a JSON object"));
        } else {
          json_object_foreach(json_object_get(j_params, "data-format"), field, j_element) {
            if (0 == o_strcmp(field, "username") || 0 == o_strcmp(field, "name") || 0 == o_strcmp(field, "email") || 0 == o_strcmp(field, "enabled") || 0 == o_strcmp(field, "password")) {
              json_array_append_new(j_error, json_string("data-format can not have settings for properties 'username', 'name', 'email', 'enabled' or 'password'"));
            } else {
              if (json_object_get(j_element, "multiple") != NULL && !json_is_boolean(json_object_get(j_element, "multiple"))) {
                json_array_append_new(j_error, json_string("multiple is optional and must be a boolean (default: false)"));
              }
              if (json_object_get(j_element, "read") != NULL && !json_is_boolean(json_object_get(j_element, "read"))) {
                json_array_append_new(j_error, json_string("read is optional and must be a boolean (default: true)"));
              }
              if (json_object_get(j_element, "write") != NULL && !json_is_boolean(json_object_get(j_element, "write"))) {
                json_array_append_new(j_error, json_string("write is optional and must be a boolean (default: true)"));
              }
              if (json_object_get(j_element, "profile-read") != NULL && !json_is_boolean(json_object_get(j_element, "profile-read"))) {
                json_array_append_new(j_error, json_string("profile-read is optional and must be a boolean (default: false)"));
              }
              if (json_object_get(j_element, "profile-write") != NULL && !json_is_boolean(json_object_get(j_element, "profile-write"))) {
                json_array_append_new(j_error, json_string("profile-write is optional and must be a boolean (default: false)"));
              }
            }
          }
        }
      }
      if (json_object_get(j_params, "pbkdf2-iterations") != NULL && json_integer_value(json_object_get(j_params, "pbkdf2-iterations")) <= 0) {
        json_array_append_new(j_error, json_string("pbkdf2-iterations is optional and must be a positive non null integer"));
      }
    }
    if (json_array_size(j_error)) {
      j_return = json_pack("{sisO}", "result", G_ERROR_PARAM, "error", j_error);
    } else {
      j_return = json_pack("{si}", "result", G_OK);
    }
    json_decref(j_error);
  } else {
    y_log_message(Y_LOG_LEVEL_ERROR, "is_user_database_parameters_valid - Error allocating resources for j_error");
    j_return = json_pack("{si}", "result", G_ERROR_MEMORY);
  }
  return j_return;
}

static char * get_pattern_clause(struct mod_parameters * param, const char * pattern) {
  char * escape_pattern = h_escape_string_with_quotes(param->conn, pattern), * clause = NULL;
  
  if (escape_pattern != NULL) {
    clause = msprintf("IN (SELECT gu_id from " G_TABLE_USER " WHERE gu_username LIKE '%%'||%s||'%%' OR gu_name LIKE '%%'||%s||'%%' OR gu_email LIKE '%%'||%s||'%%')", escape_pattern, escape_pattern, escape_pattern);
  }
  o_free(escape_pattern);
  return clause;
}

static json_int_t get_user_nb_passwords(struct mod_parameters * param, json_int_t gu_id) {
  json_t * j_query, * j_result = NULL;
  int res;
  json_int_t result = 0;
  
  j_query = json_pack("{sss[s]s{sI}}",
                      "table",
                      G_TABLE_USER_PASSWORD,
                      "columns",
                        "COUNT(guw_password) AS nb_passwords",
                      "where",
                        "gu_id",
                        gu_id);
  res = h_select(param->conn, j_query, &j_result, NULL);
  json_decref(j_query);
  if (res == H_OK) {
    if (json_array_size(j_result)) {
      result = json_integer_value(json_object_get(json_array_get(j_result, 0), "nb_passwords"));
    }
    json_decref(j_result);
  } else {
    y_log_message(Y_LOG_LEVEL_ERROR, "get_user_nb_passwords database - Error executing j_query");
  }
  return result;
}

static int append_user_properties(struct mod_parameters * param, json_t * j_user, int profile) {
  json_t * j_query, * j_result, * j_element = NULL, * j_param_config;
  int res, ret;
  size_t index = 0;
  
  if (param->conn->type == HOEL_DB_TYPE_MARIADB) {
    j_query = json_pack("{sss[ssss]s{sO}}",
                        "table",
                        G_TABLE_USER_PROPERTY,
                        "columns",
                          "gup_name AS name",
                          "gup_value_tiny AS value_tiny",
                          "gup_value_small AS value_small",
                          "gup_value_medium AS value_medium",
                        "where",
                          "gu_id",
                          json_object_get(j_user, "gu_id"));
    res = h_select(param->conn, j_query, &j_result, NULL);
    json_decref(j_query);
    if (res == H_OK) {
      json_array_foreach(j_result, index, j_element) {
        j_param_config = json_object_get(json_object_get(param->j_params, "data-format"), json_string_value(json_object_get(j_element, "name")));
        if ((!profile && json_object_get(j_param_config, "read") != json_false()) || (profile && json_object_get(j_param_config, "profile-read") != json_false())) {
          if (json_object_get(j_element, "value_tiny") != json_null()) {
            if (json_object_get(j_param_config, "multiple") == json_true()) {
              if (json_object_get(j_user, json_string_value(json_object_get(j_element, "name"))) == NULL) {
                json_object_set_new(j_user, json_string_value(json_object_get(j_element, "name")), json_array());
              }
              json_array_append(json_object_get(j_user, json_string_value(json_object_get(j_element, "name"))), json_object_get(j_element, "value_tiny"));
            } else {
              json_object_set(j_user, json_string_value(json_object_get(j_element, "name")), json_object_get(j_element, "value_tiny"));
            }
          } else if (json_object_get(j_element, "value_small") != json_null()) {
            if (json_object_get(j_param_config, "multiple") == json_true()) {
              if (json_object_get(j_user, json_string_value(json_object_get(j_element, "name"))) == NULL) {
                json_object_set_new(j_user, json_string_value(json_object_get(j_element, "name")), json_array());
              }
              json_array_append(json_object_get(j_user, json_string_value(json_object_get(j_element, "name"))), json_object_get(j_element, "value_small"));
            } else {
              json_object_set(j_user, json_string_value(json_object_get(j_element, "name")), json_object_get(j_element, "value_small"));
            }
          } else if (json_object_get(j_element, "value_medium") != json_null()) {
            if (json_object_get(j_param_config, "multiple") == json_true()) {
              if (json_object_get(j_user, json_string_value(json_object_get(j_element, "name"))) == NULL) {
                json_object_set_new(j_user, json_string_value(json_object_get(j_element, "name")), json_array());
              }
              json_array_append(json_object_get(j_user, json_string_value(json_object_get(j_element, "name"))), json_object_get(j_element, "value_medium"));
            } else {
              json_object_set(j_user, json_string_value(json_object_get(j_element, "name")), json_object_get(j_element, "value_medium"));
            }
          } else {
            if (json_object_get(j_param_config, "multiple") == json_true()) {
              if (json_object_get(j_user, json_string_value(json_object_get(j_element, "name"))) == NULL) {
                json_object_set_new(j_user, json_string_value(json_object_get(j_element, "name")), json_array());
              }
              json_array_append(json_object_get(j_user, json_string_value(json_object_get(j_element, "name"))), json_null());
            } else {
              json_object_set(j_user, json_string_value(json_object_get(j_element, "name")), json_null());
            }
          }
        }
      }
      ret = G_OK;
      json_decref(j_result);
    } else {
      y_log_message(Y_LOG_LEVEL_ERROR, "append_user_properties database - Error executing j_query");
      ret = G_ERROR_DB;
    }
  } else {
    j_query = json_pack("{sss[ss]s{sO}}",
                        "table",
                        G_TABLE_USER_PROPERTY,
                        "columns",
                          "gup_name AS name",
                          "gup_value AS value",
                        "where",
                          "gu_id",
                          json_object_get(j_user, "gu_id"));
    res = h_select(param->conn, j_query, &j_result, NULL);
    json_decref(j_query);
    if (res == H_OK) {
      json_array_foreach(j_result, index, j_element) {
        j_param_config = json_object_get(json_object_get(param->j_params, "data-format"), json_string_value(json_object_get(j_element, "name")));
        if ((!profile && json_object_get(j_param_config, "read") != json_false()) || (profile && json_object_get(j_param_config, "profile-read") != json_false())) {
          if (json_object_get(j_element, "value") != json_null()) {
            if (json_object_get(j_param_config, "multiple") == json_true()) {
              if (json_object_get(j_user, json_string_value(json_object_get(j_element, "name"))) == NULL) {
                json_object_set_new(j_user, json_string_value(json_object_get(j_element, "name")), json_array());
              }
              json_array_append(json_object_get(j_user, json_string_value(json_object_get(j_element, "name"))), json_object_get(j_element, "value"));
            } else {
              json_object_set(j_user, json_string_value(json_object_get(j_element, "name")), json_object_get(j_element, "value"));
            }
          } else {
            if (json_object_get(j_param_config, "multiple") == json_true()) {
              if (json_object_get(j_user, json_string_value(json_object_get(j_element, "name"))) == NULL) {
                json_object_set_new(j_user, json_string_value(json_object_get(j_element, "name")), json_array());
              }
              json_array_append(json_object_get(j_user, json_string_value(json_object_get(j_element, "name"))), json_null());
            } else {
              json_object_set(j_user, json_string_value(json_object_get(j_element, "name")), json_null());
            }
          }
        }
      }
      ret = G_OK;
      json_decref(j_result);
    } else {
      y_log_message(Y_LOG_LEVEL_ERROR, "append_user_properties database - Error executing j_query");
      ret = G_ERROR_DB;
    }
  }
  return ret;
}

static json_t * database_user_scope_get(struct mod_parameters * param, json_int_t gu_id) {
  json_t * j_query, * j_result, * j_return, * j_array, * j_scope = NULL;
  int res;
  size_t index = 0;
  char * scope_clause = msprintf("IN (SELECT gus_id from " G_TABLE_USER_SCOPE_USER " WHERE gu_id = %"JSON_INTEGER_FORMAT")", gu_id);
  
  j_query = json_pack("{sss[s]s{s{ssss}}ss}",
                      "table",
                      G_TABLE_USER_SCOPE,
                      "columns",
                        "gus_name AS name",
                      "where",
                        "gus_id",
                          "operator",
                          "raw",
                          "value",
                          scope_clause,
                      "order_by",
                      "gus_id");
  o_free(scope_clause);
  res = h_select(param->conn, j_query, &j_result, NULL);
  json_decref(j_query);
  if (res == H_OK) {
    j_array = json_array();
    if (j_array != NULL) {
      json_array_foreach(j_result, index, j_scope) {
        json_array_append(j_array, json_object_get(j_scope, "name"));
      }
      j_return = json_pack("{sisO}", "result", G_OK, "scope", j_array);
      json_decref(j_array);
    } else {
      y_log_message(Y_LOG_LEVEL_ERROR, "database_user_scope_get database - Error allocating resources for j_array");
      j_return = json_pack("{si}", "result", G_ERROR_MEMORY);
    }
    json_decref(j_result);
  } else {
    y_log_message(Y_LOG_LEVEL_ERROR, "database_user_scope_get database - Error executing j_query");
    j_return = json_pack("{si}", "result", G_ERROR_DB);
  }
  return j_return;
}

static json_t * database_user_get(const char * username, void * cls, int profile) {
  struct mod_parameters * param = (struct mod_parameters *)cls;
  json_t * j_query, * j_result, * j_scope, * j_return;
  int res;
  char * username_escaped, * username_clause;
  
  username_escaped = h_escape_string_with_quotes(param->conn, username);
  username_clause = msprintf(" = UPPER(%s)", username_escaped);
  j_query = json_pack("{sss[sssss]s{s{ssss}}}",
                      "table",
                      G_TABLE_USER,
                      "columns",
                        "gu_id",
                        "gu_username AS username",
                        "gu_name AS name",
                        "gu_email AS email",
                        "gu_enabled",
                      "where",
                        "UPPER(gu_username)",
                          "operator",
                          "raw",
                          "value",
                          username_clause);
  o_free(username_clause);
  o_free(username_escaped);
  res = h_select(param->conn, j_query, &j_result, NULL);
  json_decref(j_query);
  if (res == H_OK) {
    if (json_array_size(j_result)) {
      j_scope = database_user_scope_get(param, json_integer_value(json_object_get(json_array_get(j_result, 0), "gu_id")));
      if (check_result_value(j_scope, G_OK)) {
        json_object_set(json_array_get(j_result, 0), "scope", json_object_get(j_scope, "scope"));
        json_object_set(json_array_get(j_result, 0), "enabled", (json_integer_value(json_object_get(json_array_get(j_result, 0), "gu_enabled"))?json_true():json_false()));
        if (param->multiple_passwords) {
          json_object_set_new(json_array_get(j_result, 0), "password", json_integer(get_user_nb_passwords(param, json_integer_value(json_object_get(json_array_get(j_result, 0), "gu_id")))));
        }
        if (append_user_properties(param, json_array_get(j_result, 0), profile) != G_OK) {
          y_log_message(Y_LOG_LEVEL_ERROR, "database_user_get database - Error append_user_properties");
        }
        json_object_del(json_array_get(j_result, 0), "gu_enabled");
        json_object_del(json_array_get(j_result, 0), "gu_id");
        j_return = json_pack("{sisO}", "result", G_OK, "user", json_array_get(j_result, 0));
      } else {
        j_return = json_pack("{si}", "result", G_ERROR);
        y_log_message(Y_LOG_LEVEL_ERROR, "database_user_get database - Error database_user_scope_get");
      }
      json_decref(j_scope);
    } else {
      j_return = json_pack("{si}", "result", G_ERROR_NOT_FOUND);
    }
    json_decref(j_result);
  } else {
    j_return = json_pack("{si}", "result", G_ERROR_DB);
    y_log_message(Y_LOG_LEVEL_ERROR, "database_user_get database - Error executing j_query");
  }
  return j_return;
}

static char * get_password_clause_write(struct mod_parameters * param, const char * password) {
  char * clause = NULL, * password_encoded, digest[1024] = {0};
  
  if (!o_strlen(password)) {
    clause = o_strdup("''");
  } else if (param->conn->type == HOEL_DB_TYPE_SQLITE) {
    if (generate_digest_pbkdf2(password, param->PBKDF2_iterations, NULL, digest)) {
      clause = msprintf("'%s%c%u'", digest, G_PBKDF2_ITERATOR_SEP, param->PBKDF2_iterations);
    } else {
      y_log_message(Y_LOG_LEVEL_ERROR, "get_password_clause_write database - Error generate_digest_pbkdf2");
    }
  } else if (param->conn->type == HOEL_DB_TYPE_MARIADB) {
    password_encoded = h_escape_string_with_quotes(param->conn, password);
    if (password_encoded != NULL) {
      clause = msprintf("PASSWORD(%s)", password_encoded);
      o_free(password_encoded);
    } else {
      y_log_message(Y_LOG_LEVEL_ERROR, "get_password_clause_write database - Error h_escape_string_with_quotes (mariadb)");
    }
  } else if (param->conn->type == HOEL_DB_TYPE_PGSQL) {
    password_encoded = h_escape_string_with_quotes(param->conn, password);
    if (password_encoded != NULL) {
      clause = msprintf("crypt(%s, gen_salt('bf'))", password_encoded);
      o_free(password_encoded);
    } else {
      y_log_message(Y_LOG_LEVEL_ERROR, "get_password_clause_write database - Error h_escape_string_with_quotes (postgre)");
    }
  }
  return clause;
}

static int update_password_list(struct mod_parameters * param, json_int_t gu_id, const char ** new_passwords, size_t new_passwords_len, int add) {
  json_t * j_query, * j_result;
  int res, ret;
  size_t i;
  char * clause_password;
  
  if (add) {
    j_query = json_pack("{sss[]}",
                        "table",
                        G_TABLE_USER_PASSWORD,
                        "values");
    for (i=0; i<new_passwords_len; i++) {
      clause_password = get_password_clause_write(param, new_passwords[i]);
      json_array_append_new(json_object_get(j_query, "values"), json_pack("{sIs{ss}}", "gu_id", gu_id, "guw_password", "raw", clause_password));
      o_free(clause_password);
    }
    res = h_insert(param->conn, j_query, NULL);
    json_decref(j_query);
    if (res == H_OK) {
      ret = G_OK;
    } else {
      y_log_message(Y_LOG_LEVEL_ERROR, "update_password_list - Error executing j_query (1)");
      ret = G_ERROR_DB;
    }
  } else {
    j_query = json_pack("{sss[s]s{sI}}",
                        "table", G_TABLE_USER_PASSWORD,
                        "columns",
                          "guw_password",
                        "where",
                          "gu_id", gu_id);
    res = h_select(param->conn, j_query, &j_result, NULL);
    json_decref(j_query);
    if (res == H_OK) {
      j_query = json_pack("{sss{sI}}",
                          "table",
                          G_TABLE_USER_PASSWORD,
                          "where",
                            "gu_id", gu_id);
      res = h_delete(param->conn, j_query, NULL);
      json_decref(j_query);
      if (res == H_OK) {
        j_query = json_pack("{sss[]}",
                            "table",
                            G_TABLE_USER_PASSWORD,
                            "values");
        for (i=0; i<new_passwords_len; i++) {
          if (o_strlen(new_passwords[i])) {
            clause_password = get_password_clause_write(param, new_passwords[i]);
            json_array_append_new(json_object_get(j_query, "values"), json_pack("{sIs{ss}}", "gu_id", gu_id, "guw_password", "raw", clause_password));
            o_free(clause_password);
          } else if (new_passwords[i] != NULL) {
            json_array_append_new(json_object_get(j_query, "values"), json_pack("{sIsO}", "gu_id", gu_id, "guw_password", json_object_get(json_array_get(j_result, i), "guw_password")));
          }
        }
        res = h_insert(param->conn, j_query, NULL);
        json_decref(j_query);
        if (res == H_OK) {
          ret = G_OK;
        } else {
          y_log_message(Y_LOG_LEVEL_ERROR, "update_password_list - Error executing j_query (4)");
          ret = G_ERROR_DB;
        }
      } else {
        y_log_message(Y_LOG_LEVEL_ERROR, "update_password_list - Error executing j_query (3)");
        ret = G_ERROR_DB;
      }
      json_decref(j_result);
    } else {
      y_log_message(Y_LOG_LEVEL_ERROR, "update_password_list - Error executing j_query (2)");
      ret = G_ERROR_DB;
    }
  }
  return ret;
}

static char ** get_salt_from_password_hash(struct mod_parameters * param, const char * username, json_t * j_iterations) {
  json_t * j_query, * j_result, * j_element = 0;
  int res;
  unsigned char password_b64_decoded[1024] = {0};
  char * salt = NULL, * username_escaped, * username_clause, ** salt_list = NULL, * str_iterator;
  size_t password_b64_decoded_len, index = 0, gc_password_len;
  
  if (j_iterations != NULL) {
    username_escaped = h_escape_string_with_quotes(param->conn, username);
    username_clause = msprintf("IN (SELECT gu_id FROM "G_TABLE_USER" WHERE UPPER(gu_username) = UPPER(%s))", username_escaped);  
    j_query = json_pack("{sss[s]s{s{ssss}}}",
                        "table",
                        G_TABLE_USER_PASSWORD,
                        "columns",
                          "guw_password",
                        "where",
                          "gu_id",
                            "operator",
                            "raw",
                            "value",
                            username_clause);
    o_free(username_clause);
    o_free(username_escaped);
    res = h_select(param->conn, j_query, &j_result, NULL);
    json_decref(j_query);
    if (res == H_OK) {
      if (json_array_size(j_result)) {
        if ((salt_list = o_malloc((json_array_size(j_result)+1)*sizeof(char *))) != NULL) {
          json_array_foreach(j_result, index, j_element) {
            if ((str_iterator = o_strchr(json_string_value(json_object_get(j_element, "guw_password")), G_PBKDF2_ITERATOR_SEP)) != NULL) {
              gc_password_len = o_strchr(json_string_value(json_object_get(j_element, "guw_password")), G_PBKDF2_ITERATOR_SEP) - json_string_value(json_object_get(j_element, "guw_password"));
              json_array_append_new(j_iterations, json_integer(strtol(str_iterator+1, NULL, 10)));
            } else {
              gc_password_len = json_string_length(json_object_get(j_element, "guw_password"));
              json_array_append_new(j_iterations, json_integer(0));
            }
            if (json_string_length(json_object_get(j_element, "guw_password")) && o_base64_decode((const unsigned char *)json_string_value(json_object_get(j_element, "guw_password")), gc_password_len, password_b64_decoded, &password_b64_decoded_len)) {
              if ((salt = o_strdup((const char *)password_b64_decoded + password_b64_decoded_len - GLEWLWYD_DEFAULT_SALT_LENGTH)) != NULL) {
                salt_list[index] = salt;
              } else {
                y_log_message(Y_LOG_LEVEL_ERROR, "get_salt_from_password_hash - Error extracting salt");
              }
            } else {
              y_log_message(Y_LOG_LEVEL_ERROR, "get_salt_from_password_hash - Error o_base64_decode");
            }
          }
          salt_list[json_array_size(j_result)] = NULL;
        } else {
          y_log_message(Y_LOG_LEVEL_ERROR, "get_salt_from_password_hash - Error allocatig resources for salt_list (1)");
        }
      } else {
        if ((salt_list = o_malloc(sizeof(char *))) != NULL) {
          salt_list[0] = NULL;
        } else {
          y_log_message(Y_LOG_LEVEL_ERROR, "get_salt_from_password_hash - Error allocatig resources for salt_list (2)");
        }
      }
      json_decref(j_result);
    } else {
      y_log_message(Y_LOG_LEVEL_ERROR, "get_salt_from_password_hash - Error executing j_query");
    }
  } else {
    y_log_message(Y_LOG_LEVEL_ERROR, "get_salt_from_password_hash - Error j_iterations is NULL");
  }
  return salt_list;
}

static char * get_password_clause_check(struct mod_parameters * param, const char * username, const char * password) {
  char * clause = NULL, * password_encoded, digest[1024] = {0}, ** salt_list, * username_escaped = h_escape_string_with_quotes(param->conn, username);
  size_t i;
  json_t * j_iterations = json_array();
  unsigned int iterations;
  
  if (param->conn->type == HOEL_DB_TYPE_SQLITE) {
    if ((salt_list = get_salt_from_password_hash(param, username, j_iterations)) != NULL) {
      clause = o_strdup("IN (");
      for (i=0; salt_list[i]!=NULL; i++) {
        iterations = (unsigned int)json_integer_value(json_array_get(j_iterations, i));
        if (generate_digest_pbkdf2(password, iterations?iterations:1000, salt_list[i], digest)) {
          if (!i) {
            if (iterations) {
              clause = mstrcatf(clause, "'%s%c%u'", digest, G_PBKDF2_ITERATOR_SEP, iterations);
            } else {
              clause = mstrcatf(clause, "'%s'", digest);
            }
          } else {
            if (iterations) {
              clause = mstrcatf(clause, ",'%s%c%u'", digest, G_PBKDF2_ITERATOR_SEP, iterations);
            } else {
              clause = mstrcatf(clause, ",'%s'", digest);
            }
          }
          digest[0] = '\0';
        } else {
          y_log_message(Y_LOG_LEVEL_ERROR, "get_password_clause_check database - Error generate_digest_pbkdf2");
        }
      }
      clause = mstrcatf(clause, ")");
    } else {
      y_log_message(Y_LOG_LEVEL_ERROR, "get_password_clause_check database - Error get_salt_from_password_hash");
    }
    free_string_array(salt_list);
  } else if (param->conn->type == HOEL_DB_TYPE_MARIADB) {
    password_encoded = h_escape_string_with_quotes(param->conn, password);
    if (password_encoded != NULL) {
      clause = msprintf("= PASSWORD(%s)", password_encoded);
      o_free(password_encoded);
    } else {
      y_log_message(Y_LOG_LEVEL_ERROR, "get_password_clause_check database - Error h_escape_string_with_quotes (mariadb)");
    }
  } else if (param->conn->type == HOEL_DB_TYPE_PGSQL) {
    password_encoded = h_escape_string_with_quotes(param->conn, password);
    if (password_encoded != NULL) {
      clause = msprintf("= crypt(%s, guw_password)", password_encoded);
      o_free(password_encoded);
    } else {
      y_log_message(Y_LOG_LEVEL_ERROR, "get_password_clause_check database - Error h_escape_string_with_quotes (postgre)");
    }
  }
  o_free(username_escaped);
  json_decref(j_iterations);
  return clause;
}

static json_t * get_property_value_db(struct mod_parameters * param, const char * name, json_t * j_property, json_int_t gu_id) {
  if (param->conn->type == HOEL_DB_TYPE_MARIADB) {
    if (json_string_length(j_property) < 512) {
      return json_pack("{sIsssOsOsO}", "gu_id", gu_id, "gup_name", name, "gup_value_tiny", j_property, "gup_value_small", json_null(), "gup_value_medium", json_null());
    } else if (json_string_length(j_property) < 16*1024) {
      return json_pack("{sIsssOsOsO}", "gu_id", gu_id, "gup_name", name, "gup_value_tiny", json_null(), "gup_value_small", j_property, "gup_value_medium", json_null());
    } else if (json_string_length(j_property) < 16*1024*1024) {
      return json_pack("{sIsssOsOsO}", "gu_id", gu_id, "gup_name", name, "gup_value_tiny", json_null(), "gup_value_small", json_null(), "gup_value_medium", j_property);
    } else {
      y_log_message(Y_LOG_LEVEL_ERROR, "get_property_value_db - Error value is too large");
      return NULL;
    }
  } else {
    return json_pack("{sIsssO}", "gu_id", gu_id, "gup_name", name, "gup_value", j_property);
  }
}

static int save_user_properties(struct mod_parameters * param, json_t * j_user, json_int_t gu_id, int profile) {
  json_t * j_property = NULL, * j_query, * j_array = json_array(), * j_format, * j_property_value = NULL;
  const char * name = NULL;
  int ret, res;
  size_t index = 0;
  
  if (j_array != NULL) {
    json_object_foreach(j_user, name, j_property) {
      if (0 != o_strcmp(name, "username") && 0 != o_strcmp(name, "name") && 0 != o_strcmp(name, "password") && 0 != o_strcmp(name, "email") && 0 != o_strcmp(name, "enabled") && 0 != o_strcmp(name, "scope")) {
        j_format = json_object_get(json_object_get(param->j_params, "data-format"), name);
        if ((!profile && json_object_get(j_format, "write") != json_false()) || (profile && json_object_get(j_format, "profile-write") == json_true())) {
          if (!json_is_array(j_property)) {
            json_array_append_new(j_array, get_property_value_db(param, name, j_property, gu_id));
          } else {
            json_array_foreach(j_property, index, j_property_value) {
              if (j_property_value != json_null()) {
                json_array_append_new(j_array, get_property_value_db(param, name, j_property_value, gu_id));
              }
            }
          }
        }
      }
    }
    // Delete old values
    j_query = json_pack("{sss{sI}}", "table", G_TABLE_USER_PROPERTY, "where", "gu_id", gu_id);
    res = h_delete(param->conn, j_query, NULL);
    json_decref(j_query);
    if (res == H_OK) {
      // Add new values
      if (json_array_size(j_array)) {
        j_query = json_pack("{sssO}", "table", G_TABLE_USER_PROPERTY, "values", j_array);
        res = h_insert(param->conn, j_query, NULL);
        json_decref(j_query);
        if (res == H_OK) {
          ret = G_OK;
        } else {
          y_log_message(Y_LOG_LEVEL_ERROR, "insert_user_properties database - Error executing j_query insert");
          ret = G_ERROR_DB;
        }
      } else {
        ret = G_OK;
      }
    } else {
      y_log_message(Y_LOG_LEVEL_ERROR, "insert_user_properties database - Error executing j_query delete");
      ret = G_ERROR_DB;
    }
    json_decref(j_array);
  } else {
    y_log_message(Y_LOG_LEVEL_ERROR, "insert_user_properties database - Error allocating resources for j_array");
    ret = G_ERROR_MEMORY;
  }
  return ret;
}

static int save_user_scope(struct mod_parameters * param, json_t * j_scope, json_int_t gu_id) {
  json_t * j_query, * j_result, * j_element = NULL, * j_new_scope_id;
  int res, ret;
  char * scope_clause;
  size_t index = 0;
  
  j_query = json_pack("{sss{sI}}", "table", G_TABLE_USER_SCOPE_USER, "where", "gu_id", gu_id);
  res = h_delete(param->conn, j_query, NULL);
  json_decref(j_query);
  if (res == H_OK) {
    ret = G_OK;
    if (json_is_array(j_scope)) {
      json_array_foreach(j_scope, index, j_element) {
        j_query = json_pack("{sss[s]s{sO}}",
                            "table",
                            G_TABLE_USER_SCOPE,
                            "columns",
                              "gus_id",
                            "where",
                              "gus_name",
                              j_element);
        res = h_select(param->conn, j_query, &j_result, NULL);
        json_decref(j_query);
        if (res == H_OK) {
          if (json_array_size(j_result)) {
            j_query = json_pack("{sss{sIsO}}",
                                "table",
                                G_TABLE_USER_SCOPE_USER,
                                "values",
                                  "gu_id",
                                  gu_id,
                                  "gus_id",
                                  json_object_get(json_array_get(j_result, 0), "gus_id"));
            res = h_insert(param->conn, j_query, NULL);
            json_decref(j_query);
            if (res != H_OK) {
              y_log_message(Y_LOG_LEVEL_ERROR, "save_user_scope database - Error executing j_query insert scope_user (1)");
            }
          } else {
            j_query = json_pack("{sss{sO}}",
                                "table",
                                G_TABLE_USER_SCOPE,
                                "values",
                                  "gus_name",
                                  j_element);
            res = h_insert(param->conn, j_query, NULL);
            json_decref(j_query);
            if (res == H_OK) {
              j_new_scope_id = h_last_insert_id(param->conn);
              if (j_new_scope_id != NULL) {
                j_query = json_pack("{sss{sIsO}}",
                                    "table",
                                    G_TABLE_USER_SCOPE_USER,
                                    "values",
                                      "gu_id",
                                      gu_id,
                                      "gus_id",
                                      j_new_scope_id);
                res = h_insert(param->conn, j_query, NULL);
                json_decref(j_query);
                if (res != H_OK) {
                  y_log_message(Y_LOG_LEVEL_ERROR, "save_user_scope database - Error executing j_query insert scope_user (2)");
                }
              } else {
                y_log_message(Y_LOG_LEVEL_ERROR, "save_user_scope database - Error h_last_insert_id");
              }
              json_decref(j_new_scope_id);
            } else {
              y_log_message(Y_LOG_LEVEL_ERROR, "save_user_scope database - Error executing j_query insert scope");
            }
          }
          json_decref(j_result);
        } else {
          y_log_message(Y_LOG_LEVEL_ERROR, "save_user_scope database - Error executing j_query select scope");
        }
      }
    }
    // Clean orphan user_scope
    scope_clause = msprintf("NOT IN (SELECT DISTINCT(gus_id) FROM " G_TABLE_USER_SCOPE_USER ")");
    j_query = json_pack("{sss{s{ssss}}}",
                        "table",
                        G_TABLE_USER_SCOPE,
                        "where",
                          "gus_id",
                            "operator",
                            "raw",
                            "value",
                            scope_clause);
    o_free(scope_clause);
    res = h_delete(param->conn, j_query, NULL);
    json_decref(j_query);
    if (res != H_OK) {
      y_log_message(Y_LOG_LEVEL_ERROR, "save_user_scope database - Error executing j_query delete empty scopes");
    }
  } else {
    y_log_message(Y_LOG_LEVEL_ERROR, "save_user_scope database - Error executing j_query delete");
    ret = G_ERROR_DB;
  }
  
  return ret;
}

json_t * user_module_load(struct config_module * config) {
  UNUSED(config);
  return json_pack("{si ss ss ss sf}",
                   "result", G_OK,
                   "name", "database",
                   "display_name", "Database backend user module",
                   "description", "Module to store users in the database",
                   "api_version", 2.5);
}

int user_module_unload(struct config_module * config) {
  UNUSED(config);
  return G_OK;
}

json_t * user_module_init(struct config_module * config, int readonly, int multiple_passwords, json_t * j_parameters, void ** cls) {
  UNUSED(readonly);
  json_t * j_result, * j_return;
  char * error_message;
  
  j_result = is_user_database_parameters_valid(j_parameters);
  if (check_result_value(j_result, G_OK)) {
    *cls = o_malloc(sizeof(struct mod_parameters));
    if (*cls != NULL) {
      ((struct mod_parameters *)*cls)->j_params = json_incref(j_parameters);
      ((struct mod_parameters *)*cls)->hash_algorithm = config->hash_algorithm;
      ((struct mod_parameters *)*cls)->multiple_passwords = multiple_passwords;
      if (json_object_get(j_parameters, "use-glewlwyd-connection") != json_false()) {
          ((struct mod_parameters *)*cls)->use_glewlwyd_connection = 0;
          ((struct mod_parameters *)*cls)->conn = config->conn;
      } else {
        ((struct mod_parameters *)*cls)->use_glewlwyd_connection = 1;
        if (0 == o_strcmp(json_string_value(json_object_get(j_parameters, "connection-type")), "sqlite")) {
          ((struct mod_parameters *)*cls)->conn = h_connect_sqlite(json_string_value(json_object_get(j_parameters, "sqlite-dbpath")));
        } else if (0 == o_strcmp(json_string_value(json_object_get(j_parameters, "connection-type")), "mariadb")) {
          ((struct mod_parameters *)*cls)->conn = h_connect_mariadb(json_string_value(json_object_get(j_parameters, "mariadb-host")), json_string_value(json_object_get(j_parameters, "mariadb-user")), json_string_value(json_object_get(j_parameters, "mariadb-password")), json_string_value(json_object_get(j_parameters, "mariadb-dbname")), json_integer_value(json_object_get(j_parameters, "mariadb-port")), NULL);
        } else if (0 == o_strcmp(json_string_value(json_object_get(j_parameters, "connection-type")), "postgre")) {
          ((struct mod_parameters *)*cls)->conn = h_connect_pgsql(json_string_value(json_object_get(j_parameters, "postgre-conninfo")));
        }
      }
      if (((struct mod_parameters *)*cls)->conn != NULL) {
        j_return = json_pack("{si}", "result", G_OK);
      } else {
        y_log_message(Y_LOG_LEVEL_ERROR, "user_module_init database - Error connecting to database");
        j_return = json_pack("{sis[s]}", "result", G_ERROR_PARAM, "error", "Error connecting to database");
      }
      if (json_object_get(j_parameters, "pbkdf2-iterations") != NULL) {
        ((struct mod_parameters *)*cls)->PBKDF2_iterations = (unsigned int)json_integer_value(json_object_get(j_parameters, "pbkdf2-iterations"));
      } else {
        ((struct mod_parameters *)*cls)->PBKDF2_iterations = G_PBKDF2_ITERATOR_DEFAULT;
      }
    } else {
      y_log_message(Y_LOG_LEVEL_ERROR, "user_module_init database - Error allocating resources for cls");
      j_return = json_pack("{si}", "result", G_ERROR_MEMORY);
    }
  } else if (check_result_value(j_result, G_ERROR_PARAM)) {
    error_message = json_dumps(json_object_get(j_result, "error"), JSON_COMPACT);
    y_log_message(Y_LOG_LEVEL_ERROR, "user_module_init database - Error parsing parameters");
    y_log_message(Y_LOG_LEVEL_ERROR, error_message);
    o_free(error_message);
    j_return = json_pack("{sisO}", "result", G_ERROR_PARAM, "error", json_object_get(j_result, "error"));
  } else {
    y_log_message(Y_LOG_LEVEL_ERROR, "user_module_init database - Error is_user_database_parameters_valid");
    j_return = json_pack("{sis[s]}", "result", G_ERROR, "error", "internal error");
  }
  json_decref(j_result);
  return j_return;
}

int user_module_close(struct config_module * config, void * cls) {
  UNUSED(config);
  int ret;
  
  if (cls != NULL) {
    if (((struct mod_parameters *)cls)->use_glewlwyd_connection) {
      if (h_close_db(((struct mod_parameters *)cls)->conn) != H_OK) {
        y_log_message(Y_LOG_LEVEL_ERROR, "user_module_close database - Error h_close_db");
        ret = G_ERROR_DB;
      } else {
        ret = G_OK;
      }
    } else {
      ret = G_OK;
    }
    json_decref(((struct mod_parameters *)cls)->j_params);
    o_free(cls);
  } else {
    ret = G_ERROR_PARAM;
  }
  return ret;
}

size_t user_module_count_total(struct config_module * config, const char * pattern, void * cls) {
  UNUSED(config);
  struct mod_parameters * param = (struct mod_parameters *)cls;
  json_t * j_query, * j_result = NULL;
  int res;
  size_t ret = 0;
  char * pattern_clause;
  
  j_query = json_pack("{sss[s]}",
                      "table",
                      G_TABLE_USER,
                      "columns",
                        "count(gu_id) AS total");
  if (o_strlen(pattern)) {
    pattern_clause = get_pattern_clause(param, pattern);
    json_object_set_new(j_query, "where", json_pack("{s{ssss}}", "gu_id", "operator", "raw", "value", pattern_clause));
    o_free(pattern_clause);
  }
  res = h_select(param->conn, j_query, &j_result, NULL);
  json_decref(j_query);
  if (res == H_OK) {
    ret = (size_t)json_integer_value(json_object_get(json_array_get(j_result, 0), "total"));
    json_decref(j_result);
  } else {
    y_log_message(Y_LOG_LEVEL_ERROR, "user_module_count_total database - Error executing j_query");
  }
  return ret;
}

json_t * user_module_get_list(struct config_module * config, const char * pattern, size_t offset, size_t limit, void * cls) {
  UNUSED(config);
  struct mod_parameters * param = (struct mod_parameters *)cls;
  json_t * j_query, * j_result, * j_element = NULL, * j_scope, * j_return;
  int res;
  char * pattern_clause;
  size_t index = 0;
  
  j_query = json_pack("{sss[sssss]sisiss}",
                      "table",
                      G_TABLE_USER,
                      "columns",
                        "gu_id",
                        "gu_username AS username",
                        "gu_name AS name",
                        "gu_email AS email",
                        "gu_enabled",
                      "offset",
                      offset,
                      "limit",
                      limit,
                      "order_by",
                      "gu_username");
  if (o_strlen(pattern)) {
    pattern_clause = get_pattern_clause(param, pattern);
    json_object_set_new(j_query, "where", json_pack("{s{ssss}}", "gu_id", "operator", "raw", "value", pattern_clause));
    o_free(pattern_clause);
  }
  res = h_select(param->conn, j_query, &j_result, NULL);
  json_decref(j_query);
  if (res == H_OK) {
    json_array_foreach(j_result, index, j_element) {
      j_scope = database_user_scope_get(param, json_integer_value(json_object_get(j_element, "gu_id")));
      if (check_result_value(j_scope, G_OK)) {
        json_object_set(j_element, "scope", json_object_get(j_scope, "scope"));
        json_object_set(j_element, "enabled", (json_integer_value(json_object_get(j_element, "gu_enabled"))?json_true():json_false()));
        if (param->multiple_passwords) {
          json_object_set_new(j_element, "password", json_integer(get_user_nb_passwords(param, json_integer_value(json_object_get(j_element, "gu_id")))));
        }
        if (append_user_properties(param, j_element, 0) != G_OK) {
          y_log_message(Y_LOG_LEVEL_ERROR, "user_module_get_list database - Error append_user_properties");
        }
        json_object_del(j_element, "gu_enabled");
        json_object_del(j_element, "gu_id");
      } else {
        j_return = json_pack("{si}", "result", G_ERROR);
        y_log_message(Y_LOG_LEVEL_ERROR, "user_module_get_list database - Error database_user_scope_get");
      }
      json_decref(j_scope);
    }
    j_return = json_pack("{sisO}", "result", G_OK, "list", j_result);
    json_decref(j_result);
  } else {
    j_return = json_pack("{si}", "result", G_ERROR_DB);
    y_log_message(Y_LOG_LEVEL_ERROR, "user_module_get_list database - Error executing j_query");
  }
  return j_return;
}

json_t * user_module_get(struct config_module * config, const char * username, void * cls) {
  UNUSED(config);
  return database_user_get(username, cls, 0);
}

json_t * user_module_get_profile(struct config_module * config, const char * username, void * cls) {
  UNUSED(config);
  return database_user_get(username, cls, 1);
}

json_t * user_module_is_valid(struct config_module * config, const char * username, json_t * j_user, int mode, void * cls) {
  struct mod_parameters * param = (struct mod_parameters *)cls;
  json_t * j_result = json_array(), * j_element, * j_format, * j_value, * j_return = NULL, * j_cur_user;
  char * message;
  size_t index = 0;
  const char * property;
  
  if (j_result != NULL) {
    if (mode == GLEWLWYD_IS_VALID_MODE_ADD) {
      if (!json_is_string(json_object_get(j_user, "username")) || json_string_length(json_object_get(j_user, "username")) > 128) {
        json_array_append_new(j_result, json_string("username is mandatory and must be a string (maximum 128 characters)"));
      } else {
        j_cur_user = user_module_get(config, json_string_value(json_object_get(j_user, "username")), cls);
        if (check_result_value(j_cur_user, G_OK)) {
          json_array_append_new(j_result, json_string("username already exist"));
        } else if (!check_result_value(j_cur_user, G_ERROR_NOT_FOUND)) {
          y_log_message(Y_LOG_LEVEL_ERROR, "user_module_is_valid database - Error user_module_get");
        }
        json_decref(j_cur_user);
      }
    } else if ((mode == GLEWLWYD_IS_VALID_MODE_UPDATE || mode == GLEWLWYD_IS_VALID_MODE_UPDATE_PROFILE) && username == NULL) {
      json_array_append_new(j_result, json_string("username is mandatory on update mode"));
    }
    if (mode != GLEWLWYD_IS_VALID_MODE_UPDATE_PROFILE) {
      if (json_object_get(j_user, "scope") != NULL) {
        if (!json_is_array(json_object_get(j_user, "scope"))) {
          json_array_append_new(j_result, json_string("scope must be a JSON array of string"));
        } else {
          json_array_foreach(json_object_get(j_user, "scope"), index, j_element) {
            if (!json_is_string(j_element) || !json_string_length(j_element)) {
              json_array_append_new(j_result, json_string("scope must be a JSON array of string"));
            }
          }
        }
      }
    }
    if (param->multiple_passwords) {
      if (mode != GLEWLWYD_IS_VALID_MODE_UPDATE_PROFILE && json_object_get(j_user, "password") != NULL && !json_is_array(json_object_get(j_user, "password"))) {
        json_array_append_new(j_result, json_string("password must be an array"));
      }
    } else {
      if (mode != GLEWLWYD_IS_VALID_MODE_UPDATE_PROFILE && json_object_get(j_user, "password") != NULL && !json_is_string(json_object_get(j_user, "password"))) {
        json_array_append_new(j_result, json_string("password must be a string"));
      }
    }
    if (json_object_get(j_user, "name") != NULL && (!json_is_string(json_object_get(j_user, "name")) || json_string_length(json_object_get(j_user, "name")) > 256)) {
      json_array_append_new(j_result, json_string("name must be a string (maximum 256 characters)"));
    }
    if (json_object_get(j_user, "email") != NULL && (!json_is_string(json_object_get(j_user, "email")) || json_string_length(json_object_get(j_user, "email")) > 512)) {
      json_array_append_new(j_result, json_string("email must be a string (maximum 512 characters)"));
    }
    if (json_object_get(j_user, "enabled") != NULL && !json_is_boolean(json_object_get(j_user, "enabled"))) {
      json_array_append_new(j_result, json_string("enabled must be a boolean"));
    }
    json_object_foreach(j_user, property, j_element) {
      if (0 != o_strcmp(property, "username") && 0 != o_strcmp(property, "name") && 0 != o_strcmp(property, "email") && 0 != o_strcmp(property, "enabled") && 0 != o_strcmp(property, "password") && 0 != o_strcmp(property, "source") && 0 != o_strcmp(property, "scope")) {
        j_format = json_object_get(json_object_get(param->j_params, "data-format"), property);
        if (json_object_get(j_format, "multiple") == json_true()) {
          if (!json_is_array(j_element)) {
            message = msprintf("property '%s' must be a JSON array", property);
            json_array_append_new(j_result, json_string(message));
            o_free(message);
          } else {
            json_array_foreach(j_element, index, j_value) {
              if (!json_is_string(j_value) || json_string_length(j_value) > 16*1024*1024) {
                message = msprintf("property '%s' must contain a string value (maximum 16M characters)", property);
                json_array_append_new(j_result, json_string(message));
                o_free(message);
              }
            }
          }
        } else {
          if (!json_is_string(j_element) || json_string_length(j_element) > 16*1024*1024) {
            message = msprintf("property '%s' must be a string value (maximum 16M characters)", property);
            json_array_append_new(j_result, json_string(message));
            o_free(message);
          }
        }
      }
    }
    if (json_array_size(j_result)) {
      j_return = json_pack("{sisO}", "result", G_ERROR_PARAM, "error", j_result);
    } else {
      j_return = json_pack("{si}", "result", G_OK);
    }
    json_decref(j_result);
  } else {
    y_log_message(Y_LOG_LEVEL_ERROR, "user_module_is_valid database - Error allocating resources for j_result");
    j_return = json_pack("{si}", "result", G_ERROR_MEMORY);
  }
  return j_return;
}

int user_module_add(struct config_module * config, json_t * j_user, void * cls) {
  UNUSED(config);
  struct mod_parameters * param = (struct mod_parameters *)cls;
  json_t * j_query, * j_gu_id;
  int res, ret;
  const char ** passwords = NULL;
  size_t i;
  
  j_query = json_pack("{sss{ss}}",
                      "table",
                      G_TABLE_USER,
                      "values",
                        "gu_username",
                        json_string_value(json_object_get(j_user, "username")));
  
  if (json_object_get(j_user, "name") != NULL) {
    json_object_set(json_object_get(j_query, "values"), "gu_name", json_object_get(j_user, "name"));
  }
  if (json_object_get(j_user, "email") != NULL) {
    json_object_set(json_object_get(j_query, "values"), "gu_email", json_object_get(j_user, "email"));
  }
  if (json_object_get(j_user, "enabled") != NULL) {
    json_object_set_new(json_object_get(j_query, "values"), "gu_enabled", json_object_get(j_user, "enabled")==json_false()?json_integer(0):json_integer(1));
  }
  res = h_insert(param->conn, j_query, NULL);
  json_decref(j_query);
  if (res == H_OK) {
    j_gu_id = h_last_insert_id(param->conn);
    if (save_user_properties(param, j_user, json_integer_value(j_gu_id), 0) != G_OK) {
      y_log_message(Y_LOG_LEVEL_ERROR, "user_module_add database - Error save_user_properties");
      ret = G_ERROR_DB;
    } else if (json_object_get(j_user, "scope") != NULL && save_user_scope(param, json_object_get(j_user, "scope"), json_integer_value(j_gu_id)) != G_OK) {
      y_log_message(Y_LOG_LEVEL_ERROR, "user_module_add database - Error save_user_scope");
      ret = G_ERROR_DB;
    } else {
      if (param->multiple_passwords) {
        if ((passwords = o_malloc(json_array_size(json_object_get(j_user, "password"))*sizeof(char *))) != NULL) {
          for (i=0; i<json_array_size(json_object_get(j_user, "password")); i++) {
            passwords[i] = json_string_value(json_array_get(json_object_get(j_user, "password"), i));
          }
          ret = update_password_list(param, json_integer_value(j_gu_id), passwords, json_array_size(json_object_get(j_user, "password")), 1);
          o_free(passwords);
        } else {
          y_log_message(Y_LOG_LEVEL_ERROR, "user_module_add database - Error allocating resources for password");
          ret = G_ERROR_MEMORY;
        }
      } else {
        if ((passwords = o_malloc(sizeof(char *))) != NULL) {
          passwords[0] = json_string_value(json_object_get(j_user, "password"));
          ret = update_password_list(param, json_integer_value(j_gu_id), passwords, 1, 1);
          o_free(passwords);
        } else {
          y_log_message(Y_LOG_LEVEL_ERROR, "user_module_add database - Error allocating resources for password");
          ret = G_ERROR_MEMORY;
        }
      }
    }
    json_decref(j_gu_id);
  } else {
    y_log_message(Y_LOG_LEVEL_ERROR, "user_module_add database - Error executing j_query insert");
    ret = G_ERROR_DB;
  }
  return ret;
}

int user_module_update(struct config_module * config, const char * username, json_t * j_user, void * cls) {
  UNUSED(config);
  struct mod_parameters * param = (struct mod_parameters *)cls;
  json_t * j_query, * j_result = NULL;
  int res, ret;
  char * username_escaped, * username_clause;
  const char ** passwords = NULL;
  size_t i;
  
  username_escaped = h_escape_string_with_quotes(param->conn, username);
  username_clause = msprintf(" = UPPER(%s)", username_escaped);  
  j_query = json_pack("{sss[s]s{s{ssss}}}", "table", G_TABLE_USER, "columns", "gu_id", "where", "UPPER(gu_username)", "operator", "raw", "value", username_clause);
  res = h_select(param->conn, j_query, &j_result, NULL);
  json_decref(j_query);
  o_free(username_clause);
  o_free(username_escaped);
  if (res == H_OK && json_array_size(j_result)) {
    j_query = json_pack("{sss{}s{sO}}",
                        "table",
                        G_TABLE_USER,
                        "set",
                        "where",
                          "gu_id",
                          json_object_get(json_array_get(j_result, 0), "gu_id"));
    
    if (json_object_get(j_user, "name") != NULL) {
      json_object_set(json_object_get(j_query, "set"), "gu_name", json_object_get(j_user, "name"));
    }
    if (json_object_get(j_user, "email") != NULL) {
      json_object_set(json_object_get(j_query, "set"), "gu_email", json_object_get(j_user, "email"));
    }
    if (json_object_get(j_user, "enabled") != NULL) {
      json_object_set_new(json_object_get(j_query, "set"), "gu_enabled", json_object_get(j_user, "enabled")==json_false()?json_integer(0):json_integer(1));
    }
    if (json_object_size(json_object_get(j_query, "set"))) {
      res = h_update(param->conn, j_query, NULL);
    } else {
      res = H_OK;
    }
    json_decref(j_query);
    if (res == H_OK) {
      if (save_user_properties(param, j_user, json_integer_value(json_object_get(json_array_get(j_result, 0), "gu_id")), 0) != G_OK) {
        y_log_message(Y_LOG_LEVEL_ERROR, "user_module_add database - Error save_user_properties");
        ret = G_ERROR_DB;
      } else if (json_object_get(j_user, "scope") != NULL && save_user_scope(param, json_object_get(j_user, "scope"), json_integer_value(json_object_get(json_array_get(j_result, 0), "gu_id"))) != G_OK) {
        y_log_message(Y_LOG_LEVEL_ERROR, "user_module_add database - Error save_user_scope");
        ret = G_ERROR_DB;
      } else {
        if (param->multiple_passwords) {
          if ((passwords = o_malloc(json_array_size(json_object_get(j_user, "password"))*sizeof(char *))) != NULL) {
            for (i=0; i<json_array_size(json_object_get(j_user, "password")); i++) {
              passwords[i] = json_string_value(json_array_get(json_object_get(j_user, "password"), i));
            }
            ret = update_password_list(param, json_integer_value(json_object_get(json_array_get(j_result, 0), "gu_id")), passwords, json_array_size(json_object_get(j_user, "password")), 0);
            o_free(passwords);
          } else {
            y_log_message(Y_LOG_LEVEL_ERROR, "user_module_add database - Error allocating resources for password");
            ret = G_ERROR_MEMORY;
          }
        } else {
          if ((passwords = o_malloc(sizeof(char *))) != NULL) {
            passwords[0] = json_string_value(json_object_get(j_user, "password"));
            ret = update_password_list(param, json_integer_value(json_object_get(json_array_get(j_result, 0), "gu_id")), passwords, 1, 0);
            o_free(passwords);
          } else {
            y_log_message(Y_LOG_LEVEL_ERROR, "user_module_add database - Error allocating resources for password");
            ret = G_ERROR_MEMORY;
          }
        }
        ret = G_OK;
      }
    } else {
      y_log_message(Y_LOG_LEVEL_ERROR, "user_module_add database - Error executing j_query update");
      ret = G_ERROR_DB;
    }
  } else {
    ret = G_ERROR_NOT_FOUND;
  }
  json_decref(j_result);
  return ret;
}

int user_module_update_profile(struct config_module * config, const char * username, json_t * j_user, void * cls) {
  UNUSED(config);
  struct mod_parameters * param = (struct mod_parameters *)cls;
  json_t * j_query, * j_result = NULL;
  int res, ret;
  char * username_escaped, * username_clause;
  
  username_escaped = h_escape_string_with_quotes(param->conn, username);
  username_clause = msprintf(" = UPPER(%s)", username_escaped);  
  j_query = json_pack("{sss[s]s{s{ssss}}}", "table", G_TABLE_USER, "columns", "gu_id", "where", "UPPER(gu_username)", "operator", "raw", "value", username_clause);
  res = h_select(param->conn, j_query, &j_result, NULL);
  json_decref(j_query);
  o_free(username_clause);
  o_free(username_escaped);
  if (res == H_OK) {
    if (json_array_size(j_result)) {
      j_query = json_pack("{sss{}sO}",
                          "table",
                          G_TABLE_USER,
                          "set",
                          "where",
                            json_array_get(j_result, 0));
      
      if (json_object_get(j_user, "name") != NULL) {
        json_object_set(json_object_get(j_query, "set"), "gu_name", json_object_get(j_user, "name"));
      }
      if (json_object_size(json_object_get(j_query, "set"))) {
        if (h_update(param->conn, j_query, NULL) == H_OK) {
          ret = G_OK;
        } else {
          y_log_message(Y_LOG_LEVEL_ERROR, "user_module_update_profile database - Error executing j_query update");
          ret = G_ERROR_DB;
        }
      } else {
        res = G_OK;
      }
      json_decref(j_query);
      if (res == H_OK) {
        if (save_user_properties(param, j_user, json_integer_value(json_object_get(json_array_get(j_result, 0), "gu_id")), 0) != G_OK) {
          y_log_message(Y_LOG_LEVEL_ERROR, "user_module_update_profile database - Error save_user_properties");
          ret = G_ERROR_DB;
        } else {
          ret = G_OK;
        }
      }
    } else {
      y_log_message(Y_LOG_LEVEL_ERROR, "user_module_update_profile database - Error username '%s' not found", username);
      ret = G_ERROR_NOT_FOUND;
    }
  } else {
    y_log_message(Y_LOG_LEVEL_ERROR, "user_module_update_profile database - Error executing j_query select");
    ret = G_ERROR_DB;
  }
  json_decref(j_result);
  return ret;
}

int user_module_delete(struct config_module * config, const char * username, void * cls) {
  UNUSED(config);
  struct mod_parameters * param = (struct mod_parameters *)cls;
  json_t * j_query;
  int res, ret;
  char * username_escaped, * username_clause;
  
  username_escaped = h_escape_string_with_quotes(param->conn, username);
  username_clause = msprintf(" = UPPER(%s)", username_escaped);  
  j_query = json_pack("{sss{s{ssss}}}",
                      "table",
                      G_TABLE_USER,
                      "where",
                        "UPPER(gu_username)",
                          "operator",
                          "raw",
                          "value",
                          username_clause);
  o_free(username_clause);
  o_free(username_escaped);
  res = h_delete(param->conn, j_query, NULL);
  json_decref(j_query);
  if (res == H_OK) {
    ret = G_OK;
  } else {
    y_log_message(Y_LOG_LEVEL_ERROR, "user_module_delete database - Error executing j_query");
    ret = G_ERROR_DB;
  }
  return ret;
}

int user_module_check_password(struct config_module * config, const char * username, const char * password, void * cls) {
  UNUSED(config);
  struct mod_parameters * param = (struct mod_parameters *)cls;
  int ret, res;
  json_t * j_query, * j_result;
  char * clause = get_password_clause_check(param, username, password);
  char * username_escaped, * username_clause;
  
  username_escaped = h_escape_string_with_quotes(param->conn, username);
  username_clause = msprintf("IN (SELECT gu_id FROM "G_TABLE_USER" WHERE UPPER(gu_username) = UPPER(%s))", username_escaped);  
  j_query = json_pack("{sss[s]s{s{ssss}s{ssss}}}",
                      "table",
                      G_TABLE_USER_PASSWORD,
                      "columns",
                        "gu_id",
                      "where",
                        "gu_id",
                          "operator",
                          "raw",
                          "value",
                          username_clause,
                        "guw_password",
                          "operator",
                          "raw",
                          "value",
                          clause);
  o_free(clause);
  o_free(username_clause);
  o_free(username_escaped);
  res = h_select(param->conn, j_query, &j_result, NULL);
  json_decref(j_query);
  if (res == H_OK) {
    if (json_array_size(j_result)) {
      ret = G_OK;
    } else {
      ret = G_ERROR_UNAUTHORIZED;
    }
    json_decref(j_result);
  } else {
    y_log_message(Y_LOG_LEVEL_ERROR, "user_module_check_password database - Error executing j_query");
    ret = G_ERROR_DB;
  }
  return ret;
}

int user_module_update_password(struct config_module * config, const char * username, const char ** new_passwords, size_t new_passwords_len, void * cls) {
  UNUSED(config);
  struct mod_parameters * param = (struct mod_parameters *)cls;
  json_t * j_query, * j_result;
  int res, ret;
  char * username_escaped, * username_clause;
  
  username_escaped = h_escape_string_with_quotes(param->conn, username);
  username_clause = msprintf(" = UPPER(%s)", username_escaped);  
  j_query = json_pack("{sss[s]s{s{ssss}}}",
                      "table",
                      G_TABLE_USER,
                      "columns",
                        "gu_id",
                      "where",
                        "UPPER(gu_username)",
                          "operator",
                          "raw",
                          "value",
                          username_clause);
  o_free(username_clause);
  o_free(username_escaped);
  res = h_select(param->conn, j_query, &j_result, NULL);
  json_decref(j_query);
  if (res == H_OK) {
    if (json_array_size(j_result)) {
      ret = update_password_list(param, json_integer_value(json_object_get(json_array_get(j_result, 0), "gu_id")), new_passwords, new_passwords_len, 0);
    } else {
      ret = G_ERROR_UNAUTHORIZED;
    }
    json_decref(j_result);
  } else {
    y_log_message(Y_LOG_LEVEL_ERROR, "user_module_update_password database - Error executing j_query");
    ret = G_ERROR_DB;
  }
  
  return ret;
}
