/**
 *
 * Glewlwyd SSO Server
 *
 * Authentiation server
 * Users are authenticated via various backend available: database, ldap
 * Using various authentication methods available: password, OTP, send code, etc.
 * 
 * TLS client certificate authentication scheme module
 * 
 * Copyright 2019-2020 Nicolas Mora <mail@babelouest.org>
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

#include <gnutls/gnutls.h>
#include <gnutls/x509.h>
#include <gnutls/abstract.h>
#include <gnutls/pkcs12.h>
#include <string.h>
#include <jansson.h>
#include <yder.h>
#include <orcania.h>
#include "glewlwyd-common.h"

#define GLEWLWYD_SCHEME_CERTIFICATE_TABLE_USER_CERTIFICATE "gs_user_certificate"

#define G_CERT_SOURCE_TLS    0x01
#define G_CERT_SOURCE_HEADER 0x10

int user_auth_scheme_module_validate(struct config_module * config, const struct _u_request * http_request, const char * username, json_t * j_scheme_data, void * cls);

static int add_user_certificate_scheme_storage(struct config_module * config, json_t * j_parameters, const char * x509_data, const char * username, const char * user_agent);

static json_t * parse_certificate(const char * x509_data, int der_format);

static json_t * get_user_certificate_from_id_scheme_storage(struct config_module * config, json_t * j_parameters, const char * username, const char * cert_id);

/**
 * 
 * How-To generate a cert chain with cient certificates
 * 
 * OpenSSL
 * =======
 * 
 * Root cert/key
 * openssl genrsa -out root.key 4096
 * openssl req -x509 -new -nodes -key root.key -sha256 -days 1024 -out root.crt
 * 
 * Client cert/key/pfx
 * openssl genrsa -out client.key 4096
 * openssl req -new -key client.key -out client.csr
 * openssl x509 -req -in client.csr -CA root.crt -CAkey root.key -CAcreateserial -out client.crt -days 500 -sha256
 * openssl pkcs12 -export -out client.pfx -inkey client.key -in client.crt
 * 
 * GnuTLS
 * ======
 * 
 * Root cert/key
 * certtool --generate-privkey --outfile root.key --bits=4096
 * certtool --generate-request --load-privkey root.key --outfile root.csr
 * certtool --generate-self-signed --load-privkey root.key --outfile root.crt
 * 
 * Client cert/key/pfx
 * certtool --generate-privkey --outfile client.key --bits=4096
 * certtool --generate-request --load-privkey client.key --outfile client.csr
 * certtool --generate-certificate --load-request client.csr --load-ca-certificate root.crt --load-ca-privkey root.key --outfile client.crt
 * certtool --load-certificate client.crt --load-privkey client.key --to-p12 --outder --outfile client.pfx
 * 
 */

struct _cert_chain_element {
  gnutls_x509_crt_t            cert;
  char                       * dn;
  struct _cert_chain_element * issuer_cert;
  char                       * issuer_dn;
};

struct _cert_param {
  json_t                      * j_parameters;
  size_t                        cert_array_len;
  struct _cert_chain_element ** cert_array;
  ushort                        cert_source;
  pthread_mutex_t               cert_request_lock;
};

static int get_certificate_id(gnutls_x509_crt_t cert, unsigned char * cert_id, size_t * cert_id_len) {
  int ret;
  unsigned char cert_digest[64];
  size_t cert_digest_len = 64;
  gnutls_datum_t dat;
  dat.data = NULL;
  
  
  if (gnutls_x509_crt_export2(cert, GNUTLS_X509_FMT_DER, &dat) >= 0) {
    if (gnutls_fingerprint(GNUTLS_DIG_SHA256, &dat, cert_digest, &cert_digest_len) == GNUTLS_E_SUCCESS) {
      if (o_base64_encode(cert_digest, cert_digest_len, cert_id, cert_id_len)) {
        cert_id[*cert_id_len] = '\0';
        ret = G_OK;
      } else {
        y_log_message(Y_LOG_LEVEL_ERROR, "get_certificate_id - Error o_base64_encode");
        ret = G_ERROR;
      }
    } else {
      y_log_message(Y_LOG_LEVEL_ERROR, "get_certificate_id - Error gnutls_fingerprint");
      ret = G_ERROR;
    }
  } else {
    y_log_message(Y_LOG_LEVEL_ERROR, "get_certificate_id - Error gnutls_x509_crt_export2");
    ret = G_ERROR;
  }
  gnutls_free(dat.data);
  return ret;
}

static json_t * parse_certificate(const char * x509_data, int der_format) {
  json_t * j_return;
  gnutls_x509_crt_t cert = NULL;
  gnutls_datum_t cert_dat;
  char * dn = NULL, * issuer_dn = NULL;
  size_t key_id_enc_len = 256, dn_len = 0, issuer_dn_len = 0;
  time_t expires_at = 0, issued_at = 0;
  int ret;
  unsigned char * der_dec = NULL, key_id_enc[257] = {0};
  size_t der_dec_len = 0;
  
  if (o_strlen(x509_data)) {
    if (!gnutls_x509_crt_init(&cert)) {
      if (der_format) {
        cert_dat.data = NULL;
        cert_dat.size = 0;
        if (o_base64_decode((const unsigned char *)x509_data, o_strlen(x509_data), NULL, &der_dec_len)) {
          if ((der_dec = o_malloc(der_dec_len+4)) != NULL) {
            if (o_base64_decode((const unsigned char *)x509_data, o_strlen(x509_data), der_dec, &der_dec_len)) {
              cert_dat.data = der_dec;
              cert_dat.size = der_dec_len;
            } else {
              y_log_message(Y_LOG_LEVEL_ERROR, "parse_certificate - Error o_base64_decode (2)");
            }
          } else {
            y_log_message(Y_LOG_LEVEL_ERROR, "parse_certificate - Error allocating resources for der_dec");
          }
        } else {
          y_log_message(Y_LOG_LEVEL_ERROR, "parse_certificate - Error o_base64_decode (1)");
        }
      } else {
        cert_dat.data = (unsigned char *)x509_data;
        cert_dat.size = o_strlen(x509_data);
      }
      if (gnutls_x509_crt_import(cert, &cert_dat, der_format?GNUTLS_X509_FMT_DER:GNUTLS_X509_FMT_PEM) >= 0) {
        ret = gnutls_x509_crt_get_issuer_dn(cert, NULL, &issuer_dn_len);
        if (gnutls_x509_crt_get_dn(cert, NULL, &dn_len) == GNUTLS_E_SHORT_MEMORY_BUFFER && (ret == GNUTLS_E_SHORT_MEMORY_BUFFER || ret == GNUTLS_E_REQUESTED_DATA_NOT_AVAILABLE)) {
          if (ret == GNUTLS_E_SHORT_MEMORY_BUFFER) {
            if ((issuer_dn = o_malloc(issuer_dn_len +1)) != NULL) {
              if (gnutls_x509_crt_get_issuer_dn(cert, issuer_dn, &issuer_dn_len) < 0) {
                y_log_message(Y_LOG_LEVEL_ERROR, "parse_certificate - Error gnutls_x509_crt_get_issuer_dn");
                o_free(issuer_dn);
                issuer_dn = NULL;
              }
            } else {
              y_log_message(Y_LOG_LEVEL_ERROR, "parse_certificate - Error o_malloc issuer_dn");
            }
          }
          if ((dn = o_malloc(dn_len +1)) != NULL) {
            if (gnutls_x509_crt_get_dn(cert, dn, &dn_len) >= 0) {
              dn[dn_len] = '\0';
              if (get_certificate_id(cert, key_id_enc, &key_id_enc_len) == G_OK && (expires_at = gnutls_x509_crt_get_expiration_time(cert)) != (time_t)-1 && (issued_at = gnutls_x509_crt_get_activation_time(cert)) != (time_t)-1) {
                j_return = json_pack("{sis{sssisisssssissss}}",
                                     "result",
                                     G_OK,
                                     "certificate",
                                       "certificate_id",
                                       key_id_enc,
                                       "activation",
                                       issued_at,
                                       "expiration",
                                       expires_at,
                                       "certificate_dn",
                                       dn,
                                       "certificate_issuer_dn",
                                       issuer_dn!=NULL?issuer_dn:"",
                                       "last_used",
                                       0,
                                       "last_user_agent",
                                       "",
                                       "x509",
                                       x509_data);
              } else {
                y_log_message(Y_LOG_LEVEL_ERROR, "parse_certificate - Error gnutls_x509_crt_get_key_id or gnutls_x509_crt_get_expiration_time or gnutls_x509_crt_get_activation_time");
                j_return = json_pack("{si}", "result", G_ERROR);
              }
            } else {
              y_log_message(Y_LOG_LEVEL_ERROR, "parse_certificate - Error gnutls_x509_crt_get_dn (2)");
              j_return = json_pack("{si}", "result", G_ERROR);
            }
          } else {
            y_log_message(Y_LOG_LEVEL_ERROR, "parse_certificate - Error o_malloc dn");
            j_return = json_pack("{si}", "result", G_ERROR);
          }
          o_free(dn);
          o_free(issuer_dn);
        } else {
          y_log_message(Y_LOG_LEVEL_ERROR, "parse_certificate - Error gnutls_x509_crt_get_dn (1)");
          j_return = json_pack("{si}", "result", G_ERROR);
        }
      } else {
        y_log_message(Y_LOG_LEVEL_DEBUG, "parse_certificate - Error gnutls_x509_crt_import");
        j_return = json_pack("{si}", "result", G_ERROR_PARAM);
      }
      gnutls_x509_crt_deinit(cert);
    } else {
      y_log_message(Y_LOG_LEVEL_ERROR, "parse_certificate - Error gnutls_x509_crt_init");
      j_return = json_pack("{si}", "result", G_ERROR);
    }
    o_free(der_dec);
  } else {
    j_return = json_pack("{si}", "result", G_ERROR_PARAM);
  }
  return j_return;
}

static int update_user_certificate_enabled_scheme_storage(struct config_module * config, json_t * j_parameters, const char * username, const char * cert_id, int enabled) {
  json_t * j_query;
  int res, ret;
  
  j_query = json_pack("{sss{si}s{sOssss}}",
                      "table",
                      GLEWLWYD_SCHEME_CERTIFICATE_TABLE_USER_CERTIFICATE,
                      "set",
                        "gsuc_enabled",
                        enabled,
                      "where",
                        "gsuc_mod_name",
                        json_object_get(j_parameters, "mod_name"),
                        "gsuc_username",
                        username,
                        "gsuc_x509_certificate_id",
                        cert_id);
  res = h_update(config->conn, j_query, NULL);
  json_decref(j_query);
  if (res == H_OK) {
    ret = G_OK;
  } else {
    y_log_message(Y_LOG_LEVEL_DEBUG, "toggle_enabled_user_certificate_scheme_storage - Error executing j_query");
    ret = G_ERROR_DB;
  }
  return ret;
}

static int update_user_certificate_last_used_scheme_storage(struct config_module * config, json_t * j_parameters, const char * username, const char * cert_id, const char * user_agent) {
  json_t * j_query;
  int res, ret;
  char * last_used_clause;
  
  if (config->conn->type==HOEL_DB_TYPE_MARIADB) {
    last_used_clause = msprintf("FROM_UNIXTIME(%u)", time(NULL));
  } else if (config->conn->type==HOEL_DB_TYPE_PGSQL) {
    last_used_clause = msprintf("TO_TIMESTAMP(%u)", time(NULL));
  } else { // HOEL_DB_TYPE_SQLITE
    last_used_clause = msprintf("%u", time(NULL));
  }
  j_query = json_pack("{sss{s{ss}ss}s{sOssss}}",
                      "table",
                      GLEWLWYD_SCHEME_CERTIFICATE_TABLE_USER_CERTIFICATE,
                      "set",
                        "gsuc_last_used",
                          "raw",
                          last_used_clause,
                        "gsuc_last_user_agent",
                        user_agent!=NULL?user_agent:"",
                      "where",
                        "gsuc_mod_name",
                        json_object_get(j_parameters, "mod_name"),
                        "gsuc_username",
                        username,
                        "gsuc_x509_certificate_id",
                        cert_id);
  o_free(last_used_clause);
  res = h_update(config->conn, j_query, NULL);
  json_decref(j_query);
  if (res == H_OK) {
    ret = G_OK;
  } else {
    y_log_message(Y_LOG_LEVEL_DEBUG, "toggle_enabled_user_certificate_scheme_storage - Error executing j_query");
    ret = G_ERROR_DB;
  }
  return ret;
}

static int delete_user_certificate_scheme_storage(struct config_module * config, json_t * j_parameters, const char * username, const char * cert_id) {
  json_t * j_query;
  int res, ret;
  
  j_query = json_pack("{sss{sOssss}}",
                      "table",
                      GLEWLWYD_SCHEME_CERTIFICATE_TABLE_USER_CERTIFICATE,
                      "where",
                        "gsuc_mod_name",
                        json_object_get(j_parameters, "mod_name"),
                        "gsuc_username",
                        username,
                        "gsuc_x509_certificate_id",
                        cert_id);
  res = h_delete(config->conn, j_query, NULL);
  json_decref(j_query);
  if (res == H_OK) {
    ret = G_OK;
  } else {
    y_log_message(Y_LOG_LEVEL_DEBUG, "delete_user_certificate_scheme_storage - Error executing j_query");
    ret = G_ERROR_DB;
  }
  return ret;
}

static json_t * get_user_certificate_from_id_user_property(struct config_module * config, json_t * j_parameters, const char * username, const char * cert_id) {
  json_t * j_user, * j_user_certificate, * j_parsed_certificate, * j_return = NULL, * j_element = NULL;
  size_t index = 0;
  
  j_user = config->glewlwyd_module_callback_get_user(config, username);
  if (check_result_value(j_user, G_OK)) {
    j_user_certificate = json_object_get(json_object_get(j_user, "user"), json_string_value(json_object_get(j_parameters, "user-certificate-property")));
    if (json_is_string(j_user_certificate)) {
      j_parsed_certificate = parse_certificate(json_string_value(j_user_certificate), (0 == o_strcmp("DER", json_string_value(json_object_get(j_parameters, "user-certificate-format")))));
      if (check_result_value(j_parsed_certificate, G_OK)) {
        if (0 == o_strcmp(cert_id, json_string_value(json_object_get(json_object_get(j_parsed_certificate, "certificate"), "certificate_id")))) {
          j_return = json_pack("{sisO}", "result", G_OK, "certificate", json_object_get(j_parsed_certificate, "certificate"));
        } else {
          j_return = json_pack("{si}", "result", G_ERROR_NOT_FOUND);
        }
      } else {
        y_log_message(Y_LOG_LEVEL_ERROR, "get_user_certificate_from_id_user_property certificate - Error parse_certificate (1)");
        j_return = json_pack("{si}", "result", G_ERROR);
      }
      json_decref(j_parsed_certificate);
    } else if (json_is_array(j_user_certificate)) {
      json_array_foreach(j_user_certificate, index, j_element) {
        j_parsed_certificate = parse_certificate(json_string_value(j_element), (0 == o_strcmp("DER", json_string_value(json_object_get(j_parameters, "user-certificate-format")))));
        if (check_result_value(j_parsed_certificate, G_OK)) {
          if (0 == o_strcmp(cert_id, json_string_value(json_object_get(json_object_get(j_parsed_certificate, "certificate"), "certificate_id")))) {
            j_return = json_pack("{sisO}", "result", G_OK, "certificate", json_object_get(j_parsed_certificate, "certificate"));
          }
        } else {
          y_log_message(Y_LOG_LEVEL_ERROR, "get_user_certificate_from_id_user_property certificate - Error parse_certificate (2)");
        }
        json_decref(j_parsed_certificate);
      }
      if (j_return == NULL) {
        j_return = json_pack("{si}", "result", G_ERROR_NOT_FOUND);
      }
    } else {
      j_return = json_pack("{sis[]}", "result", G_OK, "certificate");
    }
  } else {
    y_log_message(Y_LOG_LEVEL_ERROR, "get_user_certificate_from_id_user_property certificate - Error glewlwyd_module_callback_get_user");
    j_return = json_pack("{si}", "result", G_ERROR);
  }
  json_decref(j_user);
  return j_return;
}

static json_t * get_user_certificate_list_user_property(struct config_module * config, json_t * j_parameters, const char * username) {
  json_t * j_user, * j_user_certificate, * j_parsed_certificate = NULL, * j_certificate_array = NULL, * j_return = NULL, * j_element = NULL, * j_user_dn = NULL;
  size_t index = 0;
  
  j_user = config->glewlwyd_module_callback_get_user(config, username);
  if (check_result_value(j_user, G_OK)) {
    if (json_string_length(json_object_get(j_parameters, "user-certificate-property"))) {
      if ((j_certificate_array = json_array()) != NULL) {
        j_user_certificate = json_object_get(json_object_get(j_user, "user"), json_string_value(json_object_get(j_parameters, "user-certificate-property")));
        if (json_is_string(j_user_certificate)) {
          j_parsed_certificate = parse_certificate(json_string_value(j_user_certificate), (0 == o_strcmp("DER", json_string_value(json_object_get(j_parameters, "user-certificate-format")))));
          if (check_result_value(j_parsed_certificate, G_OK)) {
            json_object_del(json_object_get(j_parsed_certificate, "certificate"), "x509");
            json_array_append(j_certificate_array, json_object_get(j_parsed_certificate, "certificate"));
          } else {
            y_log_message(Y_LOG_LEVEL_ERROR, "user_auth_scheme_module_can_use certificate - Error parse_certificate (1)");
          }
          json_decref(j_parsed_certificate);
        } else if (json_is_array(j_user_certificate)) {
          json_array_foreach(j_user_certificate, index, j_element) {
            j_parsed_certificate = parse_certificate(json_string_value(j_element), (0 == o_strcmp("DER", json_string_value(json_object_get(j_parameters, "user-certificate-format")))));
            if (check_result_value(j_parsed_certificate, G_OK)) {
              json_object_del(json_object_get(j_parsed_certificate, "certificate"), "x509");
              json_array_append(j_certificate_array, json_object_get(j_parsed_certificate, "certificate"));
            } else {
              y_log_message(Y_LOG_LEVEL_ERROR, "user_auth_scheme_module_can_use certificate - Error parse_certificate (2)");
            }
            json_decref(j_parsed_certificate);
          }
        }
      } else {
        y_log_message(Y_LOG_LEVEL_ERROR, "user_auth_scheme_module_can_use certificate - Error allocating resources for j_certificate_array");
      }
    }
    if (json_string_length(json_object_get(j_parameters, "user-dn-property"))) {
      j_user_dn = json_object_get(json_object_get(j_user, "user"), json_string_value(json_object_get(j_parameters, "user-dn-property")));
      if (!json_string_length(j_user_dn)) {
        j_user_dn = NULL;
      }
    }
    if (json_array_size(j_certificate_array) || json_string_length(j_user_dn)) {
      j_return = json_pack("{si}", "result", G_OK);
      if (json_array_size(j_certificate_array)) {
        json_object_set(j_return, "certificate", j_certificate_array);
      }
      if (json_string_length(j_user_dn)) {
        json_object_set(j_return, "dn", j_user_dn);
      }
    } else {
      j_return = json_pack("{si}", "result", G_ERROR_UNAUTHORIZED);
    }
    json_decref(j_certificate_array);
  } else {
    y_log_message(Y_LOG_LEVEL_ERROR, "user_auth_scheme_module_can_use certificate - Error glewlwyd_module_callback_get_user");
    j_return = json_pack("{si}", "result", G_ERROR);
  }
  json_decref(j_user);
  return j_return;
}

static json_t * get_user_certificate_from_id_scheme_storage(struct config_module * config, json_t * j_parameters, const char * username, const char * cert_id) {
  json_t * j_query, * j_result, * j_return;
  int res;

  j_query = json_pack("{sss[ssssssss]s{sOssss}}",
                      "table",
                      GLEWLWYD_SCHEME_CERTIFICATE_TABLE_USER_CERTIFICATE,
                      "columns",
                        "gsuc_x509_certificate_dn AS certificate_dn",
                        "gsuc_x509_certificate_issuer_dn AS certificate_issuer_dn",
                        "gsuc_x509_certificate_id AS certificate_id",
                        SWITCH_DB_TYPE(config->conn->type, "UNIX_TIMESTAMP(gsuc_activation) AS activation", "strftime('%s', gsuc_activation) AS activation", "EXTRACT(EPOCH FROM gsuc_activation)::integer AS activation"),
                        SWITCH_DB_TYPE(config->conn->type, "UNIX_TIMESTAMP(gsuc_expiration) AS expiration", "strftime('%s', gsuc_expiration) AS expiration", "EXTRACT(EPOCH FROM gsuc_expiration)::integer AS expiration"),
                        "gsuc_enabled",
                        SWITCH_DB_TYPE(config->conn->type, "UNIX_TIMESTAMP(gsuc_last_used) AS last_used", "strftime('%s', gsuc_last_used) AS last_used", "EXTRACT(EPOCH FROM gsuc_last_used)::integer AS last_used"),
                        "gsuc_last_user_agent AS last_user_agent",
                      "where",
                        "gsuc_mod_name",
                        json_object_get(j_parameters, "mod_name"),
                        "gsuc_username",
                        username,
                        "gsuc_x509_certificate_id",
                        cert_id);
  res = h_select(config->conn, j_query, &j_result, NULL);
  json_decref(j_query);
  if (res == H_OK) {
    if (json_array_size(j_result)) {
      if (json_integer_value(json_object_get(json_array_get(j_result, 0), "gsuc_enabled"))) {
        json_object_set(json_array_get(j_result, 0), "enabled", json_true());
      } else {
        json_object_set(json_array_get(j_result, 0), "enabled", json_false());
      }
      json_object_del(json_array_get(j_result, 0), "gsuc_enabled");
      j_return = json_pack("{sisO}", "result", G_OK, "certificate", json_array_get(j_result, 0));
    } else {
      j_return = json_pack("{si}", "result", G_ERROR_NOT_FOUND);
    }
    json_decref(j_result);
  } else {
    y_log_message(Y_LOG_LEVEL_DEBUG, "get_user_certificate_from_id_scheme_storage - Error executing j_query");
    j_return = json_pack("{si}", "result", G_ERROR_DB);
  }
  return j_return;
}

static json_t * get_user_certificate_list_scheme_storage(struct config_module * config, json_t * j_parameters, const char * username, int enabled) {
  json_t * j_query, * j_result, * j_return, * j_element = NULL;
  int res;
  size_t index = 0;
  
  j_query = json_pack("{sss[ssssssss]s{sOss}ss}",
                      "table",
                      GLEWLWYD_SCHEME_CERTIFICATE_TABLE_USER_CERTIFICATE,
                      "columns",
                        "gsuc_x509_certificate_dn AS certificate_dn",
                        "gsuc_x509_certificate_issuer_dn AS certificate_issuer_dn",
                        "gsuc_x509_certificate_id AS certificate_id",
                        SWITCH_DB_TYPE(config->conn->type, "UNIX_TIMESTAMP(gsuc_activation) AS activation", "strftime('%s', gsuc_activation) AS activation", "EXTRACT(EPOCH FROM gsuc_activation)::integer AS activation"),
                        SWITCH_DB_TYPE(config->conn->type, "UNIX_TIMESTAMP(gsuc_expiration) AS expiration", "strftime('%s', gsuc_expiration) AS expiration", "EXTRACT(EPOCH FROM gsuc_expiration)::integer AS expiration"),
                        "gsuc_enabled",
                        SWITCH_DB_TYPE(config->conn->type, "UNIX_TIMESTAMP(gsuc_last_used) AS last_used", "strftime('%s', gsuc_last_used) AS last_used", "EXTRACT(EPOCH FROM gsuc_last_used)::integer AS last_used"),
                        "gsuc_last_user_agent AS last_user_agent",
                      "where",
                        "gsuc_mod_name",
                        json_object_get(j_parameters, "mod_name"),
                        "gsuc_username",
                        username,
                      "order_by",
                      "gsuc_id");
  if (enabled) {
    json_object_set_new(json_object_get(j_query, "where"), "gsuc_enabled", json_integer(1));
  }
  res = h_select(config->conn, j_query, &j_result, NULL);
  json_decref(j_query);
  if (res == H_OK) {
    json_array_foreach(j_result, index, j_element) {
      if (json_integer_value(json_object_get(j_element, "gsuc_enabled"))) {
        json_object_set(j_element, "enabled", json_true());
      } else {
        json_object_set(j_element, "enabled", json_false());
      }
      json_object_del(j_element, "gsuc_enabled");
    }
    j_return = json_pack("{sisO}", "result", G_OK, "certificate", j_result);
    json_decref(j_result);
  } else {
    y_log_message(Y_LOG_LEVEL_ERROR, "get_user_certificate_list - Error executing j_query");
    j_return = json_pack("{si}", "result", G_ERROR_DB);
  }
  return j_return;
}

static struct _cert_chain_element * get_cert_chain_element_from_dn(struct _cert_param * cert_params, const char * dn) {
  size_t i;
  struct _cert_chain_element * cert_chain_element = NULL;
  
  for (i=0; i<cert_params->cert_array_len; i++) {
    if (0 == o_strcmp(dn, cert_params->cert_array[i]->dn)) {
      cert_chain_element = cert_params->cert_array[i];
      break;
    }
  }
  
  return cert_chain_element;
}

static int add_user_certificate_scheme_storage(struct config_module * config, json_t * j_parameters, const char * x509_data, const char * username, const char * user_agent) {
  json_t * j_query, * j_parsed_certificate, * j_result;
  char * expiration_clause, * activation_clause;
  int res, ret;
  
  if (o_strlen(x509_data)) {
    j_parsed_certificate = parse_certificate(x509_data, 0);
    if (check_result_value(j_parsed_certificate, G_OK)) {
      j_result = get_user_certificate_from_id_scheme_storage(config, j_parameters, username, json_string_value(json_object_get(json_object_get(j_parsed_certificate, "certificate"), "certificate_id")));
      if (check_result_value(j_result, G_ERROR_NOT_FOUND)) {
        if (config->conn->type==HOEL_DB_TYPE_MARIADB) {
          expiration_clause = msprintf("FROM_UNIXTIME(%"JSON_INTEGER_FORMAT")", json_integer_value(json_object_get(json_object_get(j_parsed_certificate, "certificate"), "expiration")));
          activation_clause = msprintf("FROM_UNIXTIME(%"JSON_INTEGER_FORMAT")", json_integer_value(json_object_get(json_object_get(j_parsed_certificate, "certificate"), "activation")));
        } else if (config->conn->type==HOEL_DB_TYPE_PGSQL) {
          expiration_clause = msprintf("TO_TIMESTAMP(%"JSON_INTEGER_FORMAT")", json_integer_value(json_object_get(json_object_get(j_parsed_certificate, "certificate"), "expiration")));
          activation_clause = msprintf("TO_TIMESTAMP(%"JSON_INTEGER_FORMAT")", json_integer_value(json_object_get(json_object_get(j_parsed_certificate, "certificate"), "activation")));
        } else { // HOEL_DB_TYPE_SQLITE
          expiration_clause = msprintf("%"JSON_INTEGER_FORMAT"", json_integer_value(json_object_get(json_object_get(j_parsed_certificate, "certificate"), "expiration")));
          activation_clause = msprintf("%"JSON_INTEGER_FORMAT"", json_integer_value(json_object_get(json_object_get(j_parsed_certificate, "certificate"), "activation")));
        }
        j_query = json_pack("{ss s{sO ss sO sO sO sO s{ss} s{ss} so}}",
                            "table",
                            GLEWLWYD_SCHEME_CERTIFICATE_TABLE_USER_CERTIFICATE,
                            "values",
                              "gsuc_mod_name",
                              json_object_get(j_parameters, "mod_name"),
                              "gsuc_username",
                              username,
                              "gsuc_x509_certificate_id",
                              json_object_get(json_object_get(j_parsed_certificate, "certificate"), "certificate_id"),
                              "gsuc_x509_certificate_content",
                              json_object_get(json_object_get(j_parsed_certificate, "certificate"), "x509"),
                              "gsuc_x509_certificate_dn",
                              json_object_get(json_object_get(j_parsed_certificate, "certificate"), "certificate_dn"),
                              "gsuc_x509_certificate_issuer_dn",
                              json_object_get(json_object_get(j_parsed_certificate, "certificate"), "certificate_issuer_dn"),
                              "gsuc_expiration",
                                "raw",
                                expiration_clause,
                              "gsuc_activation",
                                "raw",
                                activation_clause,
                              "gsuc_last_used",
                              json_null());
        o_free(expiration_clause);
        o_free(activation_clause);
        if (o_strlen(user_agent)) {
          json_object_set_new(json_object_get(j_query, "values"), "gsuc_last_user_agent", json_string(user_agent));
        }
        res = h_insert(config->conn, j_query, NULL);
        json_decref(j_query);
        if (res == H_OK) {
          ret = G_OK;
        } else {
          y_log_message(Y_LOG_LEVEL_ERROR, "add_user_certificate_scheme_storage - Error executing j_query");
          ret = G_ERROR_DB;
        }
      } else if (check_result_value(j_result, G_OK)) {
        y_log_message(Y_LOG_LEVEL_DEBUG, "add_user_certificate_scheme_storage - get_user_certificate_from_id_scheme_storage error param");
        ret = G_ERROR_PARAM;
      } else {
        y_log_message(Y_LOG_LEVEL_ERROR, "add_user_certificate_scheme_storage - Error get_user_certificate_from_id_scheme_storage");
        ret = G_ERROR;
      }
      json_decref(j_result);
    } else if (check_result_value(j_parsed_certificate, G_ERROR_PARAM)) {
      y_log_message(Y_LOG_LEVEL_DEBUG, "add_user_certificate_scheme_storage - parse_certificate error param");
      ret = G_ERROR_PARAM;
    } else {
      y_log_message(Y_LOG_LEVEL_ERROR, "add_user_certificate_scheme_storage - Error parse_certificate");
      ret = G_ERROR;
    }
    json_decref(j_parsed_certificate);
  } else {
    y_log_message(Y_LOG_LEVEL_DEBUG, "add_user_certificate_scheme_storage - x509 empty");
    ret = G_ERROR_PARAM;
  }

  return ret;
}

static void scm_gnutls_certificate_status_to_c_string (gnutls_certificate_status_t c_obj) {
  static const struct { 
    gnutls_certificate_status_t value; 
    const char* name; 
  } table[] =
    {
       { GNUTLS_CERT_INVALID, "invalid" },
       { GNUTLS_CERT_REVOKED, "revoked" },
       { GNUTLS_CERT_SIGNER_NOT_FOUND, "signer-not-found" },
       { GNUTLS_CERT_SIGNER_NOT_CA, "signer-not-ca" },
       { GNUTLS_CERT_INSECURE_ALGORITHM, "insecure-algorithm" },
    };
  unsigned i;
  for (i = 0; i < 5; i++)
    {
      if (table[i].value & c_obj)
        {
          y_log_message(Y_LOG_LEVEL_DEBUG, "%s", table[i].name);
        }
    }
}

static int is_certificate_valid_from_ca_chain(struct _cert_param * cert_params, gnutls_x509_crt_t cert) {
  int ret = G_OK, res;
  unsigned int result = 0;
  gnutls_x509_crt_t * cert_chain = NULL, root_x509 = NULL;
  gnutls_x509_trust_list_t tlist = NULL;
  size_t cert_chain_len = 0, issuer_dn_len = 0;
  char * issuer_dn = NULL;
  struct _cert_chain_element * cert_chain_element;
  
  if ((res = gnutls_x509_crt_get_issuer_dn(cert, NULL, &issuer_dn_len)) == GNUTLS_E_SHORT_MEMORY_BUFFER) {
    if ((issuer_dn = o_malloc(issuer_dn_len+1)) != NULL && gnutls_x509_crt_get_issuer_dn(cert, issuer_dn, &issuer_dn_len) >= 0) {
      // Calculate ca chain length
      cert_chain_len = 1;
      cert_chain_element = get_cert_chain_element_from_dn(cert_params, issuer_dn);
      while (cert_chain_element != NULL) {
        if (cert_chain_element->issuer_cert == NULL) {
          root_x509 = cert_chain_element->cert;
        }
        cert_chain_len++;
        cert_chain_element = cert_chain_element->issuer_cert;
      }
      if (root_x509 != NULL) {
        if ((cert_chain = o_malloc(cert_chain_len*sizeof(gnutls_x509_crt_t))) != NULL) {
          cert_chain[0] = cert;
          cert_chain_len = 1;
          cert_chain_element = get_cert_chain_element_from_dn(cert_params, issuer_dn);
          while (cert_chain_element != NULL) {
            cert_chain[cert_chain_len] = cert_chain_element->cert;
            cert_chain_len++;
            cert_chain_element = cert_chain_element->issuer_cert;
          }
          if (!gnutls_x509_trust_list_init(&tlist, 0)) {
            if (gnutls_x509_trust_list_add_cas(tlist, &root_x509, 1, 0) >= 0) {
              if (gnutls_x509_trust_list_verify_crt(tlist, cert_chain, cert_chain_len, 0, &result, NULL) >= 0) {
                if (!result) {
                  ret = G_OK;
                } else {
                  y_log_message(Y_LOG_LEVEL_DEBUG, "is_certificate_valid_from_ca_chain - certificate chain invalid");
                  scm_gnutls_certificate_status_to_c_string(result);
                  ret = G_ERROR_UNAUTHORIZED;
                }
              } else {
                y_log_message(Y_LOG_LEVEL_ERROR, "is_certificate_valid_from_ca_chain - Error gnutls_x509_trust_list_verify_crt");
                ret = G_ERROR;
              }
            } else {
              y_log_message(Y_LOG_LEVEL_ERROR, "is_certificate_valid_from_ca_chain - Error gnutls_x509_trust_list_add_cas");
              ret = G_ERROR;
            }
          } else {
            y_log_message(Y_LOG_LEVEL_ERROR, "is_certificate_valid_from_ca_chain - Error gnutls_x509_trust_list_init");
            ret = G_ERROR;
          }
        } else {
          y_log_message(Y_LOG_LEVEL_ERROR, "is_certificate_valid_from_ca_chain - Error allocating resources for cert_chain");
          ret = G_ERROR;
        }
        o_free(cert_chain);
      } else {
        y_log_message(Y_LOG_LEVEL_DEBUG, "is_certificate_valid_from_ca_chain - no root certificate found");
        ret = G_ERROR_UNAUTHORIZED;
      }
    } else {
      y_log_message(Y_LOG_LEVEL_ERROR, "is_certificate_valid_from_ca_chain - Error gnutls_x509_crt_get_issuer_dn (2)");
      ret = G_ERROR;
    }
  } else if (res == GNUTLS_E_REQUESTED_DATA_NOT_AVAILABLE) {
    ret = G_ERROR_UNAUTHORIZED;
  } else {
    y_log_message(Y_LOG_LEVEL_ERROR, "is_certificate_valid_from_ca_chain - Error gnutls_x509_crt_get_issuer_dn (1)");
    ret = G_ERROR;
  }
  o_free(issuer_dn);
  gnutls_x509_trust_list_deinit(tlist, 0);
  
  return ret;
}

static int is_user_certificate_valid_user_property(struct config_module * config, json_t * j_parameters, const char * username, gnutls_x509_crt_t cert) {
  json_t * j_user_list = get_user_certificate_list_user_property(config, j_parameters, username), * j_element = NULL;
  int ret;
  unsigned char key_id_enc[256] = {0};
  size_t index = 0, key_id_enc_len = 0;
  gnutls_datum_t cert_dn = {NULL, 0};
  
  if (check_result_value(j_user_list, G_OK)) {
    if (json_string_length(json_object_get(j_user_list, "dn"))) {
      if (gnutls_x509_crt_get_dn2(cert, &cert_dn) == GNUTLS_E_SUCCESS) {
        if (cert_dn.size == json_string_length(json_object_get(j_user_list, "dn")) && 
            0 == o_strncasecmp(json_string_value(json_object_get(j_user_list, "dn")), (const char *)cert_dn.data, cert_dn.size)) {
          ret = G_OK;
        } else {
          ret = G_ERROR_UNAUTHORIZED;
        }
        gnutls_free(cert_dn.data);
      } else {
        y_log_message(Y_LOG_LEVEL_ERROR, "is_user_certificate_valid_user_property - Error gnutls_x509_crt_get_dn2");
        ret = G_ERROR;
      }
    } else {
      if (get_certificate_id(cert, key_id_enc, &key_id_enc_len) == G_OK) {
        ret = G_ERROR_UNAUTHORIZED;
        json_array_foreach(json_object_get(j_user_list, "certificate"), index, j_element) {
          if (0 == o_strcmp((const char *)key_id_enc, json_string_value(json_object_get(j_element, "certificate_id")))) {
            ret = G_OK;
          }
        }
      } else {
        y_log_message(Y_LOG_LEVEL_ERROR, "is_user_certificate_valid_user_property - Error gnutls_x509_crt_get_key_id");
        ret = G_ERROR;
      }
    }
  } else {
    y_log_message(Y_LOG_LEVEL_ERROR, "is_user_certificate_valid_user_property - Error get_user_certificate_list_user_property");
    ret = G_ERROR;
  }
  json_decref(j_user_list);
  
  return ret;
}

static int is_user_certificate_valid_scheme_storage(struct config_module * config, json_t * j_parameters, const char * username, gnutls_x509_crt_t cert) {
  int ret, res;
  json_t * j_query, * j_result;
  unsigned char key_id_enc[256] = {0};
  size_t key_id_enc_len = 0;
  
  if (get_certificate_id(cert, key_id_enc, &key_id_enc_len) == G_OK) {
    key_id_enc[key_id_enc_len] = '\0';
    j_query = json_pack("{sss[s]s{sOsssssi}}",
                        "table",
                        GLEWLWYD_SCHEME_CERTIFICATE_TABLE_USER_CERTIFICATE,
                        "columns",
                          "gsuc_id",
                        "where",
                          "gsuc_mod_name",
                          json_object_get(j_parameters, "mod_name"),
                          "gsuc_username",
                          username,
                          "gsuc_x509_certificate_id",
                          key_id_enc,
                          "gsuc_enabled",
                          1);
    res = h_select(config->conn, j_query, &j_result, NULL);
    json_decref(j_query);
    if (res == H_OK) {
      if (json_array_size(j_result)) {
        ret = G_OK;
      } else {
        ret = G_ERROR_UNAUTHORIZED;
      }
      json_decref(j_result);
    } else {
      y_log_message(Y_LOG_LEVEL_ERROR, "is_user_certificate_valid_scheme_storage - Error executing j_query");
      ret = G_ERROR;
    }
  } else {
    y_log_message(Y_LOG_LEVEL_ERROR, "is_user_certificate_valid_scheme_storage - Error get_certificate_id");
    ret = G_ERROR;
  }
  
  return ret;
}

static int is_user_certificate_valid(struct config_module * config, json_t * j_parameters, const char * username, gnutls_x509_crt_t cert) {
  time_t now, exp;

  time(&now);
  exp = gnutls_x509_crt_get_expiration_time(cert);
  if (exp != (time_t)-1 && now < exp) {
    if (json_object_get(j_parameters, "use-scheme-storage") == json_true()) {
      return is_user_certificate_valid_scheme_storage(config, j_parameters, username, cert);
    } else {
      return is_user_certificate_valid_user_property(config, j_parameters, username, cert);
    }
  } else {
    y_log_message(Y_LOG_LEVEL_DEBUG, "is_user_certificate_valid - Certificate expired");
    return G_ERROR_UNAUTHORIZED;
  }
}

static json_t * identify_certificate(struct config_module * config, json_t * j_parameters, gnutls_x509_crt_t cert) {
  time_t now, exp;
  int res;
  json_t * j_query, * j_result, * j_return;
  unsigned char key_id_enc[256] = {0};
  size_t key_id_enc_len = 0;

  time(&now);
  exp = gnutls_x509_crt_get_expiration_time(cert);
  if (exp != (time_t)-1 && now < exp) {
    if (json_object_get(j_parameters, "use-scheme-storage") == json_true()) {
      if (get_certificate_id(cert, key_id_enc, &key_id_enc_len) == G_OK) {
        key_id_enc[key_id_enc_len] = '\0';
        j_query = json_pack("{sss[s]s{sOsssi}}",
                            "table",
                            GLEWLWYD_SCHEME_CERTIFICATE_TABLE_USER_CERTIFICATE,
                            "columns",
                              "gsuc_username AS username",
                            "where",
                              "gsuc_mod_name",
                              json_object_get(j_parameters, "mod_name"),
                              "gsuc_x509_certificate_id",
                              key_id_enc,
                              "gsuc_enabled",
                              1);
        res = h_select(config->conn, j_query, &j_result, NULL);
        json_decref(j_query);
        if (res == H_OK) {
          if (json_array_size(j_result) == 1) {
            j_return = json_pack("{sisO}", "result", G_OK, "username", json_object_get(json_array_get(j_result, 0), "username"));
          } else {
            j_return = json_pack("{si}", "result", G_ERROR_UNAUTHORIZED);
          }
          json_decref(j_result);
        } else {
          y_log_message(Y_LOG_LEVEL_ERROR, "identify_certificate - Error executing j_query");
          j_return = json_pack("{si}", "result", G_ERROR);
        }
      } else {
        y_log_message(Y_LOG_LEVEL_ERROR, "identify_certificate - Error get_certificate_id");
        j_return = json_pack("{si}", "result", G_ERROR);
      }
    } else {
      j_return = json_pack("{si}", "result", G_ERROR_UNAUTHORIZED);
    }
  } else {
    y_log_message(Y_LOG_LEVEL_DEBUG, "identify_certificate - Certificate expired");
    j_return = json_pack("{si}", "result", G_ERROR_UNAUTHORIZED);
  }
  return j_return;
}

static void update_cert_chain_issuer(struct _cert_chain_element ** ca_chain, size_t cert_array_len, struct _cert_chain_element * cur_ca) {
  size_t i;
  
  for (i=0; i<cert_array_len; i++) {
    if (0 == o_strcmp(ca_chain[i]->dn, cur_ca->issuer_dn)) {
      cur_ca->issuer_cert = ca_chain[i];
    }
    if (0 == o_strcmp(ca_chain[i]->issuer_dn, cur_ca->dn)) {
      ca_chain[i]->issuer_cert = cur_ca;
    }
  }
}

static int parse_ca_chain(json_t * j_ca_chain, struct _cert_chain_element *** ca_chain, size_t * cert_array_len) {
  json_t * j_element = NULL;
  size_t index = 0, len = 0;
  int ret = G_OK, cur_status, res;
  gnutls_x509_crt_t cert;
  struct _cert_chain_element * cur_ca;
  gnutls_datum_t cert_dat;
  
  *ca_chain = NULL;
  *cert_array_len = 0;
  UNUSED(j_ca_chain);
  
  if (j_ca_chain != NULL) {
    json_array_foreach(j_ca_chain, index, j_element) {
      cert = NULL;
      cur_status = G_OK;
      if (!gnutls_x509_crt_init(&cert)) {
        cert_dat.data = (unsigned char *)json_string_value(json_object_get(j_element, "cert-file"));
        cert_dat.size = json_string_length(json_object_get(j_element, "cert-file"));
        if ((res = gnutls_x509_crt_import(cert, &cert_dat, GNUTLS_X509_FMT_PEM)) < 0) {
          y_log_message(Y_LOG_LEVEL_ERROR, "parse_ca_chain - Error gnutls_x509_crt_import: %d", res);
          cur_status = G_ERROR;
        }
      } else {
        y_log_message(Y_LOG_LEVEL_ERROR, "parse_ca_chain - Error gnutls_x509_crt_init");
        cur_status = G_ERROR;
      }
      if (cur_status == G_OK) {
        cur_ca = o_malloc(sizeof(struct _cert_chain_element));
        cur_ca->cert = cert;
        cur_ca->dn = NULL;
        cur_ca->issuer_dn = NULL;
        cur_ca->issuer_cert = NULL;
        len = 0;
        if (gnutls_x509_crt_get_dn(cert, NULL, &len) == GNUTLS_E_SHORT_MEMORY_BUFFER) {
          if ((cur_ca->dn = o_malloc(len+1)) == NULL || gnutls_x509_crt_get_dn(cert, cur_ca->dn, &len) < 0) {
            y_log_message(Y_LOG_LEVEL_ERROR, "parse_ca_chain - Error gnutls_x509_crt_get_dn (2) on cert at index %zu", index);
            cur_status = G_ERROR;
          } else {
            cur_ca->dn[len] = '\0';
          }
        } else {
          y_log_message(Y_LOG_LEVEL_ERROR, "parse_ca_chain - Error gnutls_x509_crt_get_dn (1) on cert at index %zu", index);
          cur_status = G_ERROR;
        }
        if (cur_status == G_OK) {
          len = 0;
          if ((res = gnutls_x509_crt_get_issuer_dn(cert, NULL, &len)) == GNUTLS_E_SHORT_MEMORY_BUFFER) {
            if ((cur_ca->issuer_dn = o_malloc(len+1)) == NULL || gnutls_x509_crt_get_issuer_dn(cert, cur_ca->issuer_dn, &len) < 0) {
              y_log_message(Y_LOG_LEVEL_ERROR, "parse_ca_chain - Error gnutls_x509_crt_get_issuer_dn (2) on cert at index %zu", index);
              cur_status = G_ERROR;
            } else {
              cur_ca->issuer_dn[len] = '\0';
            }
          } else if (res != GNUTLS_E_REQUESTED_DATA_NOT_AVAILABLE) {
            y_log_message(Y_LOG_LEVEL_ERROR, "parse_ca_chain - Error gnutls_x509_crt_get_issuer_dn (1) on cert at index %zu", index);
            cur_status = G_ERROR;
          }
        }
        if (cur_status == G_OK) {
          update_cert_chain_issuer(*ca_chain, *cert_array_len, cur_ca);
          *ca_chain = o_realloc(*ca_chain, ((*cert_array_len)+1)*sizeof(struct _cert_chain_element *));
          if (*ca_chain != NULL) {
            (*ca_chain)[*cert_array_len] = cur_ca;
            (*cert_array_len)++;
          } else {
            y_log_message(Y_LOG_LEVEL_ERROR, "parse_ca_chain - Error alocatig resources for ca_chain at index %zu", index);
            gnutls_x509_crt_deinit(cert);
            o_free(cur_ca->issuer_dn);
            o_free(cur_ca->dn);
            o_free(cur_ca);
          }
        } else {
          gnutls_x509_crt_deinit(cert);
          o_free(cur_ca->issuer_dn);
          o_free(cur_ca->dn);
          o_free(cur_ca);
        }
      } else {
        gnutls_x509_crt_deinit(cert);
      }
      if (cur_status != G_OK) {
        ret = cur_status;
        break;
      }
    }
  }
  return ret;
}

static json_t * is_certificate_parameters_valid(json_t * j_parameters) {
  json_t * j_array = json_array(), * j_return, * j_element = NULL;
  size_t index = 0;
  
  if (j_array != NULL) {
    if (json_is_object(j_parameters)) {
      if (json_object_get(j_parameters, "cert-source") != NULL && 0 != o_strcmp("TLS", json_string_value(json_object_get(j_parameters, "cert-source"))) && 0 != o_strcmp("header", json_string_value(json_object_get(j_parameters, "cert-source"))) && 0 != o_strcmp("both", json_string_value(json_object_get(j_parameters, "cert-source")))) {
        json_array_append_new(j_array, json_string("cert-source is optional and must be one of the following values: 'TLS', 'header' or 'both'"));
      }
      if ((0 == o_strcmp("header", json_string_value(json_object_get(j_parameters, "cert-source"))) || 0 == o_strcmp("both", json_string_value(json_object_get(j_parameters, "cert-source")))) && !json_string_length(json_object_get(j_parameters, "header-name"))) {
        json_array_append_new(j_array, json_string("header-name is mandatory when cert-source is 'header' or 'both' and must be a non empty string"));
      }
      if (json_object_get(j_parameters, "use-scheme-storage") != NULL && !json_is_boolean(json_object_get(j_parameters, "use-scheme-storage"))) {
        json_array_append_new(j_array, json_string("use-scheme-storage is optional and must be a boolean"));
      }
      if (json_object_get(j_parameters, "use-scheme-storage") != json_true()) {
        if (!json_string_length(json_object_get(j_parameters, "user-certificate-property")) && !json_string_length(json_object_get(j_parameters, "user-dn-property"))) {
          json_array_append_new(j_array, json_string("user-certificate-property or user-dn-property is mandatory and must be a non empty string"));
        }
        if (json_string_length(json_object_get(j_parameters, "user-certificate-property")) &&
            json_object_get(j_parameters, "user-certificate-format") != NULL && 
            0 != o_strcmp("PEM", json_string_value(json_object_get(j_parameters, "user-certificate-format"))) && 
            0 != o_strcmp("DER", json_string_value(json_object_get(j_parameters, "user-certificate-format")))) {
          json_array_append_new(j_array, json_string("user-certificate-format is optional and must be one of the following values: 'PEM' or 'DER'"));
        }
      }
      if (json_object_get(j_parameters, "ca-chain") != NULL && !json_is_array(json_object_get(j_parameters, "ca-chain"))) {
        json_array_append_new(j_array, json_string("ca-chain is optional and must be an array of JSON objects"));
      } else {
        json_array_foreach(json_object_get(j_parameters, "ca-chain"), index, j_element) {
          if (!json_is_object(j_element) || !json_string_length(json_object_get(j_element, "file-name")) || !json_string_length(json_object_get(j_element, "cert-file"))) {
            json_array_append_new(j_array, json_string("A ca-chain object must have the format {file-name: '', cert-file: ''} with non empty string values"));
          }
        }
      }
    } else {
      json_array_append_new(j_array, json_string("certificate parameters must be a JSON object"));
    }
    if (!json_array_size(j_array)) {
      j_return = json_pack("{si}", "result", G_OK);
    } else {
      j_return = json_pack("{sisO}", "result", G_ERROR_PARAM, "error", j_array);
    }
  } else {
    y_log_message(Y_LOG_LEVEL_ERROR, "is_certificate_parameters_valid - Error allocating resources for j_array");
    j_return = json_pack("{si}", "result", G_ERROR_MEMORY);
  }
  json_decref(j_array);
  return j_return;
}

/**
 * 
 * user_auth_scheme_module_load
 * 
 * Executed once when Glewlwyd service is started
 * Used to identify the module and to show its parameters on init
 * You can also use it to load resources that are required once for all
 * instance modules for example
 * 
 * @return value: a json_t * value with the following pattern:
 *                {
 *                  result: number (G_OK on success, another value on error)
 *                  name: string, mandatory, name of the module, must be unique among other scheme modules
 *                  display_name: string, optional, long name of the module
 *                  description: string, optional, description for the module
 *                  parameters: object, optional, parameters description for the module
 *                }
 * 
 *                Example:
 *                {
 *                  result: G_OK,
 *                  name: "mock",
 *                  display_name: "Mock scheme module",
 *                  description: "Mock scheme module for glewlwyd tests",
 *                  parameters: {
 *                    mock-value: {
 *                      type: "string",
 *                      mandatory: true
 *                    }
 *                  }
 *                }
 * 
 * @parameter config: a struct config_module with acess to some Glewlwyd
 *                    service and data
 * 
 */
json_t * user_auth_scheme_module_load(struct config_module * config) {
  UNUSED(config);
  return json_pack("{si ss ss ss}",
                   "result", G_OK,
                   "name", "certificate",
                   "display_name", "Client certificate",
                   "description", "Client certificate scheme module");
}

/**
 * 
 * user_auth_scheme_module_unload
 * 
 * Executed once when Glewlwyd service is stopped
 * You can also use it to release resources that are required once for all
 * instance modules for example
 * 
 * @return value: G_OK on success, another value on error
 * 
 * @parameter config: a struct config_module with acess to some Glewlwyd
 *                    service and data
 * 
 */
int user_auth_scheme_module_unload(struct config_module * config) {
  UNUSED(config);
  return G_OK;
}

/**
 * 
 * user_auth_scheme_module_init
 * 
 * Initialize an instance of this module declared in Glewlwyd service.
 * If required, you must dynamically allocate a pointer to the configuration
 * for this instance and pass it to *cls
 * 
 * @return value: G_OK on success, another value on error
 * 
 * @parameter config: a struct config_module with acess to some Glewlwyd
 *                    service and data
 * @parameter j_parameters: used to initialize an instance in JSON format
 *                          The module must validate itself its parameters
 * @parameter mod_name: module name in glewlwyd service
 * @parameter cls: will contain an allocated void * pointer that will be sent back
 *                 as void * in all module functions
 * 
 */
json_t * user_auth_scheme_module_init(struct config_module * config, json_t * j_parameters, const char * mod_name, void ** cls) {
  UNUSED(config);
  UNUSED(mod_name);
  pthread_mutexattr_t mutexattr;
  json_t * j_result = is_certificate_parameters_valid(j_parameters), * j_return;
  
  if (check_result_value(j_result, G_OK)) {
    json_object_set_new(j_parameters, "mod_name", json_string(mod_name));
    if ((*cls = o_malloc(sizeof(struct _cert_param))) != NULL) {
      pthread_mutexattr_init ( &mutexattr );
      pthread_mutexattr_settype( &mutexattr, PTHREAD_MUTEX_RECURSIVE );
      if (!pthread_mutex_init(&((struct _cert_param *)*cls)->cert_request_lock, &mutexattr)) {
        ((struct _cert_param *)*cls)->cert_source = 0;
        ((struct _cert_param *)*cls)->cert_array_len = 0;
        ((struct _cert_param *)*cls)->cert_array = NULL;
        if (json_object_get(j_parameters, "cert-source") == NULL || 0 == o_strcmp("TLS", json_string_value(json_object_get(j_parameters, "cert-source")))) {
          ((struct _cert_param *)*cls)->cert_source = G_CERT_SOURCE_TLS;
        } else if (0 == o_strcmp("header", json_string_value(json_object_get(j_parameters, "cert-source")))) {
          ((struct _cert_param *)*cls)->cert_source = G_CERT_SOURCE_HEADER;
        } else {
          ((struct _cert_param *)*cls)->cert_source = G_CERT_SOURCE_TLS|G_CERT_SOURCE_HEADER;
        }
        if (parse_ca_chain(json_object_get(j_parameters, "ca-chain"), &(((struct _cert_param *)*cls)->cert_array), &(((struct _cert_param *)*cls)->cert_array_len)) == G_OK) {
          ((struct _cert_param *)*cls)->j_parameters = json_incref(j_parameters);
          j_return = json_pack("{si}", "result", G_OK);
        } else {
          y_log_message(Y_LOG_LEVEL_ERROR, "user_auth_scheme_module_init certificate - Error parse_ca_chain");
          o_free(*cls);
          *cls = NULL;
          j_return = json_pack("{si}", "result", G_ERROR);
        }
      } else {
        y_log_message(Y_LOG_LEVEL_ERROR, "user_auth_scheme_module_init certificate - Error pthread_mutex_init");
        o_free(*cls);
        *cls = NULL;
        j_return = json_pack("{si}", "result", G_ERROR);
      }
    } else {
      y_log_message(Y_LOG_LEVEL_ERROR, "user_auth_scheme_module_init certificate - Error allocating resources for cls");
      j_return = json_pack("{si}", "result", G_ERROR);
    }
  } else if (check_result_value(j_result, G_ERROR_PARAM)) {
    j_return = json_pack("{sisO}", "result", G_ERROR_PARAM, "error", json_object_get(j_result, "error"));
  } else {
    y_log_message(Y_LOG_LEVEL_ERROR, "user_auth_scheme_module_init certificate - Error is_certificate_parameters_valid");
    j_return = json_pack("{si}", "result", G_ERROR);
  }
  json_decref(j_result);
  return j_return;
}

/**
 * 
 * user_auth_scheme_module_close
 * 
 * Close an instance of this module declared in Glewlwyd service.
 * You must free the memory previously allocated in
 * the user_auth_scheme_module_init function as void * cls
 * 
 * @return value: G_OK on success, another value on error
 * 
 * @parameter config: a struct config_module with acess to some Glewlwyd
 *                    service and data
 * @parameter cls: pointer to the void * cls value allocated in user_auth_scheme_module_init
 * 
 */
int user_auth_scheme_module_close(struct config_module * config, void * cls) {
  UNUSED(config);
  size_t i;
  
  pthread_mutex_destroy(&((struct _cert_param *)cls)->cert_request_lock);
  json_decref(((struct _cert_param *)cls)->j_parameters);
  for (i=0; i<((struct _cert_param *)cls)->cert_array_len; i++) {
    o_free(((struct _cert_param *)cls)->cert_array[i]->dn);
    o_free(((struct _cert_param *)cls)->cert_array[i]->issuer_dn);
    gnutls_x509_crt_deinit(((struct _cert_param *)cls)->cert_array[i]->cert);
    o_free(((struct _cert_param *)cls)->cert_array[i]);
  }
  o_free(((struct _cert_param *)cls)->cert_array);
  o_free(((struct _cert_param *)cls));
  return G_OK;
}

/**
 * 
 * user_auth_scheme_module_can_use
 * 
 * Validate if the user is allowed to use this scheme prior to the
 * authentication or registration
 * 
 * @return value: GLEWLWYD_IS_REGISTERED - User can use scheme and has registered
 *                GLEWLWYD_IS_AVAILABLE - User can use scheme but hasn't registered
 *                GLEWLWYD_IS_NOT_AVAILABLE - User can't use scheme
 * 
 * @parameter config: a struct config_module with acess to some Glewlwyd
 *                    service and data
 * @parameter username: username to identify the user
 * @parameter cls: pointer to the void * cls value allocated in user_auth_scheme_module_init
 * 
 */
int user_auth_scheme_module_can_use(struct config_module * config, const char * username, void * cls) {
  UNUSED(config);
  json_t * j_user_certificate = NULL;
  int ret = GLEWLWYD_IS_NOT_AVAILABLE;
  
  if (json_object_get(((struct _cert_param *)cls)->j_parameters, "use-scheme-storage") != json_true()) {
    j_user_certificate = get_user_certificate_list_user_property(config, ((struct _cert_param *)cls)->j_parameters, username);
    ret = (check_result_value(j_user_certificate, G_OK) && (json_array_size(json_object_get(j_user_certificate, "certificate")) || json_string_length(json_object_get(j_user_certificate, "dn"))))?GLEWLWYD_IS_REGISTERED:GLEWLWYD_IS_AVAILABLE;
    json_decref(j_user_certificate);
  } else {
    j_user_certificate = get_user_certificate_list_scheme_storage(config, ((struct _cert_param *)cls)->j_parameters, username, 1);
    ret = (check_result_value(j_user_certificate, G_OK) && json_array_size(json_object_get(j_user_certificate, "certificate")))?GLEWLWYD_IS_REGISTERED:GLEWLWYD_IS_AVAILABLE;
    json_decref(j_user_certificate);
  }
  return ret;
}

/**
 * 
 * user_auth_scheme_module_register
 * 
 * Register the scheme for a user
 * Ex: add a certificate, add new TOTP values, etc.
 * 
 * @return value: a json_t * value with the following pattern:
 *                {
 *                  result: number (G_OK on success, another value on error)
 *                  response: JSON object, optional
 *                }
 * 
 * @parameter config: a struct config_module with acess to some Glewlwyd
 *                    service and data
 * @parameter http_request: the original struct _u_request from the HTTP API
 * @parameter username: username to identify the user
 * @parameter j_scheme_data: additional data used to register the scheme for the user
 *                           in JSON format
 * @parameter cls: pointer to the void * cls value allocated in user_auth_scheme_module_init
 * 
 */
json_t * user_auth_scheme_module_register(struct config_module * config, const struct _u_request * http_request, const char * username, json_t * j_scheme_data, void * cls) {
  json_t * j_return, * j_result;
  int ret, clean_cert = 0;
  char * x509_data = NULL;
  const char * header_cert = NULL;
  unsigned char key_id_enc[257] = {0};
  size_t key_id_enc_len = 256;
  gnutls_x509_crt_t cert = NULL;
  gnutls_datum_t cert_dat;
  
  if (0 == o_strcmp("test-certificate", json_string_value(json_object_get(j_scheme_data, "register")))) {
    ret = user_auth_scheme_module_validate(config, http_request, username, NULL, cls);
    if (ret == G_OK) {
      if ((((struct _cert_param *)cls)->cert_source & G_CERT_SOURCE_TLS) && http_request->client_cert != NULL) {
        cert = http_request->client_cert;
      } else if ((((struct _cert_param *)cls)->cert_source & G_CERT_SOURCE_HEADER) && (header_cert = u_map_get(http_request->map_header, json_string_value(json_object_get(((struct _cert_param *)cls)->j_parameters, "header-name")))) != NULL) {
        if (!gnutls_x509_crt_init(&cert)) {
          clean_cert = 1;
          cert_dat.data = (unsigned char *)header_cert;
          cert_dat.size = o_strlen(header_cert);
          if (gnutls_x509_crt_import(cert, &cert_dat, GNUTLS_X509_FMT_PEM) < 0) {
            y_log_message(Y_LOG_LEVEL_DEBUG, "user_auth_scheme_module_validate certificate - Error gnutls_x509_crt_import");
            ret = G_ERROR_UNAUTHORIZED;
          }
        } else {
          y_log_message(Y_LOG_LEVEL_DEBUG, "user_auth_scheme_module_validate certificate - Error gnutls_x509_crt_init");
          ret = G_ERROR_UNAUTHORIZED;
        }
        ret = G_ERROR_UNAUTHORIZED;
      }
      if (cert != NULL) {
        if (get_certificate_id(cert, key_id_enc, &key_id_enc_len) == G_OK) {
          key_id_enc[key_id_enc_len] = '\0';
          if (json_object_get(((struct _cert_param *)cls)->j_parameters, "use-scheme-storage") == json_true()) {
            j_result = get_user_certificate_from_id_scheme_storage(config, ((struct _cert_param *)cls)->j_parameters, username, (const char *)key_id_enc);
            if (check_result_value(j_result, G_OK)) {
              j_return = json_pack("{sisO}", "result", G_OK, "response", json_object_get(j_result, "certificate"));
            } else {
              y_log_message(Y_LOG_LEVEL_ERROR, "user_auth_scheme_module_register - Error get_user_certificate_from_id_scheme_storage");
              j_return = json_pack("{si}", "result", G_ERROR);
            }
            json_decref(j_result);
          } else {
            j_result = get_user_certificate_from_id_user_property(config, ((struct _cert_param *)cls)->j_parameters, username, (const char *)key_id_enc);
            if (check_result_value(j_result, G_OK)) {
              j_return = json_pack("{sisO}", "result", G_OK, "response", json_object_get(j_result, "certificate"));
            } else {
              y_log_message(Y_LOG_LEVEL_ERROR, "user_auth_scheme_module_register - Error get_user_certificate_from_id_user_property");
              j_return = json_pack("{si}", "result", G_ERROR);
            }
            json_decref(j_result);
          }
        } else {
          y_log_message(Y_LOG_LEVEL_ERROR, "user_auth_scheme_module_register - Error get_certificate_id");
          j_return = json_pack("{si}", "result", G_ERROR);
        }
        if (clean_cert) {
          gnutls_x509_crt_deinit(cert);
        }
      } else {
        j_return = json_pack("{si}", "result", G_ERROR_PARAM);
      }
    } else {
      j_return = json_pack("{si}", "result", G_ERROR_PARAM);
    }
  } else if (json_object_get(((struct _cert_param *)cls)->j_parameters, "use-scheme-storage") == json_true()) {
    if (0 == o_strcmp("upload-certificate", json_string_value(json_object_get(j_scheme_data, "register")))) {
      if ((ret = add_user_certificate_scheme_storage(config, ((struct _cert_param *)cls)->j_parameters, json_string_value(json_object_get(j_scheme_data, "x509")), username, u_map_get_case(http_request->map_header, "user-agent"))) == G_OK) {
        j_return = json_pack("{si}", "result", G_OK);
      } else if (ret == G_ERROR_PARAM) {
        j_return = json_pack("{si}", "result", G_ERROR_PARAM);
      } else {
        y_log_message(Y_LOG_LEVEL_ERROR, "user_auth_scheme_module_register certificate - Error add_user_certificate_scheme_storage (1)");
        j_return = json_pack("{si}", "result", G_ERROR);
      }
    } else if (0 == o_strcmp("use-certificate", json_string_value(json_object_get(j_scheme_data, "register")))) {
      if ((((struct _cert_param *)cls)->cert_source & G_CERT_SOURCE_TLS) && http_request->client_cert != NULL) {
        if ((x509_data = ulfius_export_client_certificate_pem(http_request)) != NULL) {
          if ((ret = add_user_certificate_scheme_storage(config, ((struct _cert_param *)cls)->j_parameters, x509_data, username, u_map_get_case(http_request->map_header, "user-agent"))) == G_OK) {
            j_return = json_pack("{si}", "result", G_OK);
          } else if (ret == G_ERROR_PARAM) {
            j_return = json_pack("{si}", "result", G_ERROR_PARAM);
          } else {
            y_log_message(Y_LOG_LEVEL_ERROR, "user_auth_scheme_module_register certificate - Error add_user_certificate_scheme_storage (2)  ");
            j_return = json_pack("{si}", "result", G_ERROR);
          }
          o_free(x509_data);
        } else {
          y_log_message(Y_LOG_LEVEL_ERROR, "user_auth_scheme_module_register certificate - Error ulfius_export_client_certificate_pem");
          j_return = json_pack("{si}", "result", G_ERROR);
        }
      } else if ((((struct _cert_param *)cls)->cert_source & G_CERT_SOURCE_HEADER) && (header_cert = u_map_get(http_request->map_header, json_string_value(json_object_get(((struct _cert_param *)cls)->j_parameters, "header-name")))) != NULL) {
        if ((ret = add_user_certificate_scheme_storage(config, ((struct _cert_param *)cls)->j_parameters, header_cert, username, u_map_get_case(http_request->map_header, "user-agent"))) == G_OK) {
          j_return = json_pack("{si}", "result", G_OK);
        } else if (ret == G_ERROR_PARAM) {
          j_return = json_pack("{si}", "result", G_ERROR_PARAM);
        } else {
          y_log_message(Y_LOG_LEVEL_ERROR, "user_auth_scheme_module_register certificate - Error add_user_certificate_scheme_storage (2)  ");
          j_return = json_pack("{si}", "result", G_ERROR);
        }
      } else {
        y_log_message(Y_LOG_LEVEL_DEBUG, "user_auth_scheme_module_register certificate - No certificate");
        j_return = json_pack("{si}", "result", G_ERROR_PARAM);
      }
    } else if (0 == o_strcmp("toggle-certificate", json_string_value(json_object_get(j_scheme_data, "register")))) {
      if (json_string_length(json_object_get(j_scheme_data, "certificate_id"))) {
        j_result = get_user_certificate_from_id_scheme_storage(config, ((struct _cert_param *)cls)->j_parameters, username, json_string_value(json_object_get(j_scheme_data, "certificate_id")));
        if (check_result_value(j_result, G_OK)) {
          if (update_user_certificate_enabled_scheme_storage(config, ((struct _cert_param *)cls)->j_parameters, username, json_string_value(json_object_get(j_scheme_data, "certificate_id")), json_object_get(j_scheme_data, "enabled") == json_true()) == G_OK) {
            j_return = json_pack("{si}", "result", G_OK);
          } else {
            y_log_message(Y_LOG_LEVEL_ERROR, "user_auth_scheme_module_register certificate - Error update_user_certificate_enabled_scheme_storage");
            j_return = json_pack("{si}", "result", G_ERROR);
          }
        } else if (check_result_value(j_result, G_ERROR_NOT_FOUND)) {
          j_return = json_pack("{si}", "result", G_ERROR_PARAM);
        } else {
          y_log_message(Y_LOG_LEVEL_ERROR, "user_auth_scheme_module_register certificate - Error get_user_certificate_from_id_scheme_storage");
          j_return = json_pack("{si}", "result", G_ERROR);
        }
        json_decref(j_result);
      } else {
        j_return = json_pack("{si}", "result", G_ERROR_PARAM);
      }
    } else if (0 == o_strcmp("delete-certificate", json_string_value(json_object_get(j_scheme_data, "register")))) {
      if (json_string_length(json_object_get(j_scheme_data, "certificate_id"))) {
        j_result = get_user_certificate_from_id_scheme_storage(config, ((struct _cert_param *)cls)->j_parameters, username, json_string_value(json_object_get(j_scheme_data, "certificate_id")));
        if (check_result_value(j_result, G_OK)) {
          if (delete_user_certificate_scheme_storage(config, ((struct _cert_param *)cls)->j_parameters, username, json_string_value(json_object_get(j_scheme_data, "certificate_id"))) == G_OK) {
            j_return = json_pack("{si}", "result", G_OK);
          } else {
            y_log_message(Y_LOG_LEVEL_ERROR, "user_auth_scheme_module_register certificate - Error delete_user_certificate_scheme_storage");
            j_return = json_pack("{si}", "result", G_ERROR);
          }
        } else if (check_result_value(j_result, G_ERROR_NOT_FOUND)) {
          j_return = json_pack("{si}", "result", G_ERROR_PARAM);
        } else {
          y_log_message(Y_LOG_LEVEL_ERROR, "user_auth_scheme_module_register certificate - Error get_user_certificate_from_id_scheme_storage");
          j_return = json_pack("{si}", "result", G_ERROR);
        }
        json_decref(j_result);
      } else {
        j_return = json_pack("{si}", "result", G_ERROR_PARAM);
      }
    } else {
      j_return = json_pack("{si}", "result", G_ERROR_PARAM);
    }
  } else {
    j_return = json_pack("{si}", "result", G_ERROR_PARAM);
  }
  
  return j_return;
}

/**
 * 
 * user_auth_scheme_module_register_get
 * 
 * Get the registration value(s) of the scheme for a user
 * 
 * @return value: a json_t * value with the following pattern:
 *                {
 *                  result: number (G_OK on success, another value on error)
 *                  response: JSON object, optional
 *                }
 * 
 * @parameter config: a struct config_module with acess to some Glewlwyd
 *                    service and data
 * @parameter http_request: the original struct _u_request from the API, must be casted to be available
 * @parameter username: username to identify the user
 * @parameter cls: pointer to the void * cls value allocated in user_auth_scheme_module_init
 * 
 */
json_t * user_auth_scheme_module_register_get(struct config_module * config, const struct _u_request * http_request, const char * username, void * cls) {
  UNUSED(http_request);
  json_t * j_return, * j_result;
  
  if (json_object_get(((struct _cert_param *)cls)->j_parameters, "use-scheme-storage") == json_true()) {
    j_result = get_user_certificate_list_scheme_storage(config, ((struct _cert_param *)cls)->j_parameters, username, 0);
    if (check_result_value(j_result, G_OK)) {
      j_return = json_pack("{sis{sOso}}", "result", G_OK, "response", "certificate", json_object_get(j_result, "certificate"), "add-certificate", (json_object_get(((struct _cert_param *)cls)->j_parameters, "use-scheme-storage")==json_true()?json_true():json_false()));
    } else {
      y_log_message(Y_LOG_LEVEL_ERROR, "user_auth_scheme_module_register_get certificate - Error get_user_certificate_list_scheme_storage");
      j_return = json_pack("{si}", "result", G_ERROR);
    }
    json_decref(j_result);
  } else {
    j_result = get_user_certificate_list_user_property(config, ((struct _cert_param *)cls)->j_parameters, username);
    if (check_result_value(j_result, G_OK)) {
      json_object_del(j_result, "result");
      json_object_set(j_result, "add-certificate", (json_object_get(((struct _cert_param *)cls)->j_parameters, "use-scheme-storage")==json_true()?json_true():json_false()));
      j_return = json_pack("{sisO}", "result", G_OK, "response", j_result);
    } else {
      y_log_message(Y_LOG_LEVEL_ERROR, "user_auth_scheme_module_register_get certificate - Error get_user_certificate_list_user_property");
      j_return = json_pack("{si}", "result", G_ERROR);
    }
    json_decref(j_result);
  }
  
  return j_return;
}

/**
 * 
 * user_auth_scheme_module_deregister
 * 
 * Deregister the scheme for a user
 * Ex: remove certificates, TOTP values, etc.
 * 
 * @return value: G_OK on success, even if no data has been removed
 *                G_ERROR on another error
 * 
 * @parameter config: a struct config_module with acess to some Glewlwyd
 *                    service and data
 * @parameter username: username to identify the user
 * @parameter cls: pointer to the void * cls value allocated in user_auth_scheme_module_init
 * 
 */
int user_auth_scheme_module_deregister(struct config_module * config, const char * username, void * cls) {
  json_t * j_result, * j_element = NULL;
  int ret;
  size_t index = 0;
  
  if (json_object_get(((struct _cert_param *)cls)->j_parameters, "use-scheme-storage") == json_true()) {
    j_result = get_user_certificate_list_scheme_storage(config, ((struct _cert_param *)cls)->j_parameters, username, 0);
    if (check_result_value(j_result, G_OK)) {
      json_array_foreach(json_object_get(j_result, "certificate"), index, j_element) {
        if (delete_user_certificate_scheme_storage(config, ((struct _cert_param *)cls)->j_parameters, username, json_string_value(json_object_get(j_element, "certificate_id"))) != G_OK) {
          y_log_message(Y_LOG_LEVEL_ERROR, "user_auth_scheme_module_register certificate - Error delete_user_certificate_scheme_storage");
        }
      }
      ret = G_OK;
    } else {
      y_log_message(Y_LOG_LEVEL_ERROR, "user_auth_scheme_module_deregister certificate - Error get_user_certificate_list_scheme_storage");
      ret = G_ERROR;
    }
    json_decref(j_result);
  } else {
    ret = G_OK;
  }
  
  return ret;
}

/**
 * 
 * user_auth_scheme_module_trigger
 * 
 * Trigger the scheme for a user
 * Ex: send the code to a device, generate a challenge, etc.
 * 
 * @return value: a json_t * value with the following pattern:
 *                {
 *                  result: number (G_OK on success, another value on error)
 *                  response: JSON object, optional
 *                }
 * 
 * @parameter config: a struct config_module with acess to some Glewlwyd
 *                    service and data
 * @parameter http_request: the original struct _u_request from the API, must be casted to be available
 * @parameter username: username to identify the user
 * @parameter scheme_trigger: data sent to trigger the scheme for the user
 *                           in JSON format
 * @parameter cls: pointer to the void * cls value allocated in user_auth_scheme_module_init
 * 
 */
json_t * user_auth_scheme_module_trigger(struct config_module * config, const struct _u_request * http_request, const char * username, json_t * j_scheme_trigger, void * cls) {
  UNUSED(config);
  UNUSED(http_request);
  UNUSED(username);
  UNUSED(j_scheme_trigger);
  UNUSED(cls);
  json_t * j_return = json_pack("{si}", "result", G_OK);
  
  return j_return;
}

/**
 * 
 * user_auth_scheme_module_validate
 * 
 * Validate the scheme for a user
 * Ex: check the code sent to a device, verify the challenge, etc.
 * 
 * @return value: G_OK on success
 *                G_ERROR_UNAUTHORIZED if validation fails
 *                G_ERROR_PARAM if error in parameters
 *                G_ERROR on another error
 * 
 * @parameter config: a struct config_module with acess to some Glewlwyd
 *                    service and data
 * @parameter http_request: the original struct _u_request from the API, must be casted to be available
 * @parameter username: username to identify the user
 * @parameter j_scheme_data: data sent to validate the scheme for the user
 *                           in JSON format
 * @parameter cls: pointer to the void * cls value allocated in user_auth_scheme_module_init
 * 
 */
int user_auth_scheme_module_validate(struct config_module * config, const struct _u_request * http_request, const char * username, json_t * j_scheme_data, void * cls) {
  UNUSED(j_scheme_data);
  int ret = G_OK, res, clean_cert = 0;
  const char * header_cert = NULL;
  gnutls_x509_crt_t cert = NULL;
  gnutls_datum_t cert_dat;
  unsigned char cert_id[257] = {};
  size_t cert_id_len = 256;

  // Get or parse certificate
  if ((((struct _cert_param *)cls)->cert_source & G_CERT_SOURCE_TLS) && http_request->client_cert != NULL) {
    cert = http_request->client_cert;
  } else if ((((struct _cert_param *)cls)->cert_source & G_CERT_SOURCE_HEADER) && (header_cert = u_map_get(http_request->map_header, json_string_value(json_object_get(((struct _cert_param *)cls)->j_parameters, "header-name")))) != NULL) {
    if (!gnutls_x509_crt_init(&cert)) {
      clean_cert = 1;
      cert_dat.data = (unsigned char *)header_cert;
      cert_dat.size = o_strlen(header_cert);
      if (gnutls_x509_crt_import(cert, &cert_dat, GNUTLS_X509_FMT_PEM) < 0) {
        y_log_message(Y_LOG_LEVEL_DEBUG, "user_auth_scheme_module_validate certificate - Error gnutls_x509_crt_import");
        ret = G_ERROR_UNAUTHORIZED;
      }
    } else {
      y_log_message(Y_LOG_LEVEL_DEBUG, "user_auth_scheme_module_validate certificate - Error gnutls_x509_crt_init");
      ret = G_ERROR;
    }
  }
  
  // Validate certificate
  if (ret == G_OK && cert != NULL) {
    if ((res = is_user_certificate_valid(config, ((struct _cert_param *)cls)->j_parameters, username, cert)) == G_OK) {
      if (((struct _cert_param *)cls)->cert_array_len) {
        ret = is_certificate_valid_from_ca_chain((struct _cert_param *)cls, cert);
        if (ret != G_OK && ret != G_ERROR_UNAUTHORIZED) {
          y_log_message(Y_LOG_LEVEL_ERROR, "user_auth_scheme_module_validate certificate - Error is_certificate_valid_from_ca_chain");
          ret = G_ERROR;
        } else if (ret == G_ERROR_UNAUTHORIZED) {
          y_log_message(Y_LOG_LEVEL_DEBUG, "user_auth_scheme_module_validate certificate - is_certificate_valid_from_ca_chain unauthorized");
        } else if (json_object_get(((struct _cert_param *)cls)->j_parameters, "use-scheme-storage") == json_true()) {
          if (get_certificate_id(cert, cert_id, &cert_id_len) == G_OK) {
            cert_id[cert_id_len] = '\0';
            if (update_user_certificate_last_used_scheme_storage(config, ((struct _cert_param *)cls)->j_parameters, username, (const char *)cert_id, u_map_get_case(http_request->map_header, "user-agent")) != G_OK) {
              y_log_message(Y_LOG_LEVEL_ERROR, "user_auth_scheme_module_validate certificate - Error update_user_certificate_last_used_scheme_storage");
              ret = G_ERROR;
            }
          } else {
            y_log_message(Y_LOG_LEVEL_ERROR, "user_auth_scheme_module_validate certificate - Error get_certificate_id");
            ret = G_ERROR;
          }
        }
      } else {
        if (json_object_get(((struct _cert_param *)cls)->j_parameters, "use-scheme-storage") == json_true()) {
          if (get_certificate_id(cert, cert_id, &cert_id_len) == G_OK) {
            cert_id[cert_id_len] = '\0';
            if (update_user_certificate_last_used_scheme_storage(config, ((struct _cert_param *)cls)->j_parameters, username, (const char *)cert_id, u_map_get_case(http_request->map_header, "user-agent")) != G_OK) {
              y_log_message(Y_LOG_LEVEL_ERROR, "user_auth_scheme_module_validate certificate - Error update_user_certificate_last_used_scheme_storage");
              ret = G_ERROR;
            }
          } else {
            y_log_message(Y_LOG_LEVEL_ERROR, "user_auth_scheme_module_validate certificate - Error get_certificate_id");
            ret = G_ERROR;
          }
        }
      }
    } else if (res == G_ERROR_UNAUTHORIZED || res == G_ERROR_PARAM) {
      y_log_message(Y_LOG_LEVEL_DEBUG, "user_auth_scheme_module_validate certificate - is_user_certificate_valid unauthorized");
      ret = G_ERROR_UNAUTHORIZED;
    } else {
      y_log_message(Y_LOG_LEVEL_ERROR, "user_auth_scheme_module_validate certificate - Error is_user_certificate_valid");
      ret = G_ERROR;
    }
    if (clean_cert) {
      gnutls_x509_crt_deinit(cert);
    }
  } else {
    y_log_message(Y_LOG_LEVEL_DEBUG, "user_auth_scheme_module_validate certificate - No certificate");
    ret = G_ERROR_UNAUTHORIZED;
  }

  return ret;
}

/**
 * 
 * user_auth_scheme_module_identify
 * 
 * Identify the user using the scheme without the username to be previously given
 * This functionality isn't available for all schemes, because the scheme authentification
 * must be triggered without username and the authentication result must contain the username
 * 
 * @return value: a json_t * value with the following pattern:
 *                {
 *                  result: number (G_OK on success, another value on error)
 *                  username: string value of the user identified - if the function is called within /auth
 *                  response: JSON object, optional - if the function is called within /auth/scheme/trigger
 *                }
 * 
 * @parameter config: a struct config_module with acess to some Glewlwyd
 *                    service and data
 * @parameter http_request: the original struct _u_request from the API, must be casted to be available
 * @parameter j_scheme_data: data sent to validate the scheme for the user
 *                           in JSON format
 * @parameter cls: pointer to the void * cls value allocated in user_auth_scheme_module_init
 * 
 */
json_t * user_auth_scheme_module_identify(struct config_module * config, const struct _u_request * http_request, json_t * j_scheme_data, void * cls) {
  UNUSED(j_scheme_data);
  int res, clean_cert = 0;
  const char * header_cert = NULL;
  gnutls_x509_crt_t cert = NULL;
  gnutls_datum_t cert_dat;
  unsigned char cert_id[257] = {};
  size_t cert_id_len = 256;
  json_t * j_result, * j_return;

  // Get or parse certificate
  if ((((struct _cert_param *)cls)->cert_source & G_CERT_SOURCE_TLS) && http_request->client_cert != NULL) {
    cert = http_request->client_cert;
  } else if ((((struct _cert_param *)cls)->cert_source & G_CERT_SOURCE_HEADER) && (header_cert = u_map_get(http_request->map_header, json_string_value(json_object_get(((struct _cert_param *)cls)->j_parameters, "header-name")))) != NULL) {
    if (!gnutls_x509_crt_init(&cert)) {
      clean_cert = 1;
      cert_dat.data = (unsigned char *)header_cert;
      cert_dat.size = o_strlen(header_cert);
      if (gnutls_x509_crt_import(cert, &cert_dat, GNUTLS_X509_FMT_PEM) < 0) {
        y_log_message(Y_LOG_LEVEL_DEBUG, "user_auth_scheme_module_identify certificate - Error gnutls_x509_crt_import");
      }
    } else {
      y_log_message(Y_LOG_LEVEL_DEBUG, "user_auth_scheme_module_identify certificate - Error gnutls_x509_crt_init");
    }
  }
  
  // Validate certificate
  if (cert != NULL) {
    j_result = identify_certificate(config, ((struct _cert_param *)cls)->j_parameters, cert);
    if (check_result_value(j_result, G_OK)) {
      if (((struct _cert_param *)cls)->cert_array_len) {
        res = is_certificate_valid_from_ca_chain((struct _cert_param *)cls, cert);
        if (res != G_OK && res != G_ERROR_UNAUTHORIZED) {
          y_log_message(Y_LOG_LEVEL_ERROR, "user_auth_scheme_module_identify certificate - Error is_certificate_valid_from_ca_chain");
          j_return = json_pack("{si}", "result", G_ERROR);
        } else if (res == G_ERROR_UNAUTHORIZED) {
          y_log_message(Y_LOG_LEVEL_DEBUG, "user_auth_scheme_module_identify certificate - is_certificate_valid_from_ca_chain unauthorized");
          j_return = json_pack("{si}", "result", G_ERROR_UNAUTHORIZED);
        } else if (json_object_get(((struct _cert_param *)cls)->j_parameters, "use-scheme-storage") == json_true()) {
          if (get_certificate_id(cert, cert_id, &cert_id_len) == G_OK) {
            cert_id[cert_id_len] = '\0';
            if (update_user_certificate_last_used_scheme_storage(config, ((struct _cert_param *)cls)->j_parameters, json_string_value(json_object_get(j_result, "username")), (const char *)cert_id, u_map_get_case(http_request->map_header, "user-agent")) != G_OK) {
              y_log_message(Y_LOG_LEVEL_ERROR, "user_auth_scheme_module_identify certificate - Error update_user_certificate_last_used_scheme_storage");
              j_return = json_pack("{si}", "result", G_ERROR);
            } else {
              j_return = json_pack("{sisO}", "result", G_OK, "username", json_object_get(j_result, "username"));
            }
          } else {
            y_log_message(Y_LOG_LEVEL_ERROR, "user_auth_scheme_module_identify certificate - Error get_certificate_id");
            j_return = json_pack("{si}", "result", G_ERROR);
          }
        } else {
          y_log_message(Y_LOG_LEVEL_DEBUG, "user_auth_scheme_module_identify certificate - use-scheme-storage isn't set");
          j_return = json_pack("{si}", "result", G_ERROR_UNAUTHORIZED);
        }
      } else {
        if (json_object_get(((struct _cert_param *)cls)->j_parameters, "use-scheme-storage") == json_true()) {
          if (get_certificate_id(cert, cert_id, &cert_id_len) == G_OK) {
            cert_id[cert_id_len] = '\0';
            if (update_user_certificate_last_used_scheme_storage(config, ((struct _cert_param *)cls)->j_parameters, json_string_value(json_object_get(j_result, "username")), (const char *)cert_id, u_map_get_case(http_request->map_header, "user-agent")) != G_OK) {
              y_log_message(Y_LOG_LEVEL_ERROR, "user_auth_scheme_module_identify certificate - Error update_user_certificate_last_used_scheme_storage");
              j_return = json_pack("{si}", "result", G_ERROR);
            } else {
              j_return = json_pack("{sisO}", "result", G_OK, "username", json_object_get(j_result, "username"));
            }
          } else {
            y_log_message(Y_LOG_LEVEL_ERROR, "user_auth_scheme_module_identify certificate - Error get_certificate_id");
            j_return = json_pack("{si}", "result", G_ERROR);
          }
        } else {
          y_log_message(Y_LOG_LEVEL_DEBUG, "user_auth_scheme_module_identify certificate - use-scheme-storage isn't set");
          j_return = json_pack("{si}", "result", G_ERROR_UNAUTHORIZED);
        }
      }
    } else if (check_result_value(j_result, G_ERROR_UNAUTHORIZED) || check_result_value(j_result, G_ERROR_PARAM)) {
      y_log_message(Y_LOG_LEVEL_DEBUG, "user_auth_scheme_module_identify certificate - is_user_certificate_valid unauthorized");
      j_return = json_pack("{si}", "result", G_ERROR_UNAUTHORIZED);
    } else {
      y_log_message(Y_LOG_LEVEL_ERROR, "user_auth_scheme_module_identify certificate - Error is_user_certificate_valid");
      j_return = json_pack("{si}", "result", G_ERROR);
    }
    json_decref(j_result);
    if (clean_cert) {
      gnutls_x509_crt_deinit(cert);
    }
  } else {
    y_log_message(Y_LOG_LEVEL_DEBUG, "user_auth_scheme_module_identify certificate - No certificate");
    j_return = json_pack("{si}", "result", G_ERROR_UNAUTHORIZED);
  }

  return j_return;
}
