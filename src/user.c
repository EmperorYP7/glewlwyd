/**
 *
 * Glewlwyd SSO Server
 *
 * Authentiation server
 * Users are authenticated via various backend available: database, ldap
 * Using various authentication methods available: password, OTP, send code, etc.
 * 
 * user management functions definition
 *
 * Copyright 2016-2021 Nicolas Mora <mail@babelouest.org>
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
#include "glewlwyd.h"

json_t * auth_check_user_credentials(struct config_elements * config, const char * username, const char * password) {
  int res;
  json_t * j_return = NULL, * j_module_list = get_user_module_list(config), * j_module, * j_user;
  struct _user_module_instance * user_module;
  size_t index;
  
  if (check_result_value(j_module_list, G_OK)) {
    json_array_foreach(json_object_get(j_module_list, "module"), index, j_module) {
      if (j_return == NULL) {
        user_module = get_user_module_instance(config, json_string_value(json_object_get(j_module, "name")));
        if (user_module != NULL) {
          if (user_module->enabled) {
            j_user = user_module->module->user_module_get(config->config_m, username, user_module->cls);
            if (check_result_value(j_user, G_OK) && json_object_get(json_object_get(j_user, "user"), "enabled") == json_true()) {
              res = user_module->module->user_module_check_password(config->config_m, username, password, user_module->cls);
              if (res == G_OK) {
                j_return = json_pack("{si}", "result", G_OK);
              } else if (res == G_ERROR_UNAUTHORIZED) {
                j_return = json_pack("{si}", "result", G_ERROR_UNAUTHORIZED);
              } else if (res != G_ERROR_NOT_FOUND) {
                y_log_message(Y_LOG_LEVEL_ERROR, "auth_check_user_credentials - Error, user_module_check_password for module '%s', skip", user_module->name);
              }
            } else if (!check_result_value(j_user, G_ERROR_NOT_FOUND)) {
              y_log_message(Y_LOG_LEVEL_ERROR, "auth_check_user_credentials - Error, user_module_get for module '%s', skip", user_module->name);
            }
            json_decref(j_user);
          }
        } else {
          y_log_message(Y_LOG_LEVEL_ERROR, "auth_check_user_credentials - Error, user_module_instance %s is NULL", json_string_value(json_object_get(j_module, "name")));
        }
      }
    }
  } else {
    y_log_message(Y_LOG_LEVEL_ERROR, "auth_check_user_credentials - Error get_user_module_list");
    j_return = json_pack("{si}", "result", G_ERROR);
  }
  if (j_return == NULL) {
    j_return = json_pack("{si}", "result", G_ERROR_UNAUTHORIZED);
  }
  json_decref(j_module_list);
  return j_return;
}

json_t * auth_check_user_scheme(struct config_elements * config, const char * scheme_type, const char * scheme_name, const char * username, json_t * j_scheme_value, const struct _u_request * request) {
  struct _user_auth_scheme_module_instance * scheme_instance;
  json_t * j_return = NULL, * j_user;
  int res;
  
  if ((res = user_has_scheme(config, username, scheme_name)) == G_OK) {
    j_user = get_user(config, username, NULL);
    if (check_result_value(j_user, G_OK) && json_object_get(json_object_get(j_user, "user"), "enabled") == json_true()) {
      scheme_instance = get_user_auth_scheme_module_instance(config, scheme_name);
      if (scheme_instance != NULL && 0 == o_strcmp(scheme_type, scheme_instance->module->name) && scheme_instance->enabled) {
        res = scheme_instance->module->user_auth_scheme_module_validate(config->config_m, request, username, j_scheme_value, scheme_instance->cls);
        if (res == G_OK || res == G_ERROR_UNAUTHORIZED || res == G_ERROR_PARAM || res == G_ERROR_NOT_FOUND || res == G_ERROR) {
          j_return = json_pack("{si}", "result", res);
        } else {
          y_log_message(Y_LOG_LEVEL_ERROR, "auth_check_user_scheme - Error unrecognize return value for user_auth_scheme_module_validate: %d", res);
          j_return = json_pack("{si}", "result", G_ERROR);
        }
      } else {
        j_return = json_pack("{si}", "result", G_ERROR_UNAUTHORIZED);
      }
    } else {
      j_return = json_pack("{si}", "result", G_ERROR_UNAUTHORIZED);
    }
    json_decref(j_user);
  } else if (res != G_ERROR_NOT_FOUND) {
    y_log_message(Y_LOG_LEVEL_ERROR, "auth_check_user_scheme - Error user_has_scheme");
    j_return = json_pack("{si}", "result", G_ERROR);
  } else {
    j_return = json_pack("{si}", "result", G_ERROR_UNAUTHORIZED);
  }
  return j_return;
}

json_t * auth_check_identify_scheme(struct config_elements * config, const char * scheme_type, const char * scheme_name, json_t * j_scheme_value, const struct _u_request * request) {
  struct _user_auth_scheme_module_instance * scheme_instance;
  json_t * j_return = NULL, * j_user, * j_response;
  
  scheme_instance = get_user_auth_scheme_module_instance(config, scheme_name);
  if (scheme_instance != NULL && 0 == o_strcmp(scheme_type, scheme_instance->module->name) && scheme_instance->enabled) {
    j_response = scheme_instance->module->user_auth_scheme_module_identify(config->config_m, request, j_scheme_value, scheme_instance->cls);
    if (check_result_value(j_response, G_OK)) {
      j_user = get_user(config, json_string_value(json_object_get(j_response, "username")), NULL);
      if (check_result_value(j_user, G_OK) && json_object_get(json_object_get(j_user, "user"), "enabled") == json_true()) {
        j_return = json_pack("{sisO}", "result", G_OK, "username", json_object_get(j_response, "username"));
      } else {
        j_return = json_pack("{si}", "result", G_ERROR_UNAUTHORIZED);
      }
      json_decref(j_user);
    } else if (!check_result_value(j_response, G_ERROR)) {
      j_return = json_incref(j_response);
    } else {
      y_log_message(Y_LOG_LEVEL_ERROR, "auth_check_identify_scheme - Error user_auth_scheme_module_identify");
      j_return = json_pack("{si}", "result", G_ERROR);
    }
    json_decref(j_response);
  } else {
    j_return = json_pack("{si}", "result", G_ERROR_UNAUTHORIZED);
  }
  return j_return;
}

json_t * auth_trigger_user_scheme(struct config_elements * config, const char * scheme_type, const char * scheme_name, const char * username, json_t * j_trigger_parameters, const struct _u_request * request) {
  struct _user_auth_scheme_module_instance * scheme_instance;
  json_t * j_return = NULL, * j_response = NULL;

  scheme_instance = get_user_auth_scheme_module_instance(config, scheme_name);
  if (scheme_instance != NULL && 0 == o_strcmp(scheme_type, scheme_instance->module->name) && scheme_instance->enabled) {
    j_response = scheme_instance->module->user_auth_scheme_module_trigger(config->config_m, request, username, j_trigger_parameters, scheme_instance->cls);
    if (check_result_value(j_response, G_OK)) {
      if (json_object_get(j_response, "response") != NULL) {
        j_return = json_pack("{sisO}", "result", G_OK, "trigger", json_object_get(j_response, "response"));
      } else {
        j_return = json_pack("{si}", "result", G_OK);
      }
    } else if (!check_result_value(j_response, G_ERROR)) {
      j_return = json_incref(j_response);
    } else {
      y_log_message(Y_LOG_LEVEL_ERROR, "auth_trigger_user_scheme - Error user_auth_scheme_module_trigger");
      j_return = json_pack("{si}", "result", G_ERROR);
    }
    json_decref(j_response);
  } else {
    j_return = json_pack("{si}", "result", G_ERROR_UNAUTHORIZED);
  }
  return j_return;
}

json_t * auth_trigger_identify_scheme(struct config_elements * config, const char * scheme_type, const char * scheme_name, json_t * j_trigger_parameters, const struct _u_request * request) {
  struct _user_auth_scheme_module_instance * scheme_instance;
  json_t * j_return = NULL, * j_response = NULL;

  scheme_instance = get_user_auth_scheme_module_instance(config, scheme_name);
  if (scheme_instance != NULL && 0 == o_strcmp(scheme_type, scheme_instance->module->name) && scheme_instance->enabled) {
    j_response = scheme_instance->module->user_auth_scheme_module_identify(config->config_m, request, j_trigger_parameters, scheme_instance->cls);
    if (check_result_value(j_response, G_OK)) {
      if (json_object_get(j_response, "response") != NULL) {
        j_return = json_pack("{sisO}", "result", G_OK, "trigger", json_object_get(j_response, "response"));
      } else {
        j_return = json_pack("{si}", "result", G_OK);
      }
    } else if (!check_result_value(j_response, G_ERROR)) {
      j_return = json_incref(j_response);
    } else {
      y_log_message(Y_LOG_LEVEL_ERROR, "auth_trigger_identify_scheme - Error user_auth_scheme_module_trigger");
      j_return = json_pack("{si}", "result", G_ERROR);
    }
    json_decref(j_response);
  } else {
    j_return = json_pack("{si}", "result", G_ERROR_UNAUTHORIZED);
  }
  return j_return;
}

json_t * auth_register_user_scheme(struct config_elements * config, const char * scheme_type, const char * scheme_name, const char * username, int delegate, json_t * j_register_parameters, const struct _u_request * request) {
  struct _user_auth_scheme_module_instance * scheme_instance;
  json_t * j_return = NULL, * j_response = NULL;
  int res;
  
  if ((res = user_has_scheme(config, username, scheme_name)) == G_OK) {
    if (json_is_object(j_register_parameters)) {
      scheme_instance = get_user_auth_scheme_module_instance(config, scheme_name);
      if (scheme_instance != NULL && 0 == o_strcmp(scheme_type, scheme_instance->module->name) && scheme_instance->enabled) {
        if (delegate || scheme_instance->guasmi_allow_user_register) {
          j_response = scheme_instance->module->user_auth_scheme_module_register(config->config_m, request, username, j_register_parameters, scheme_instance->cls);
          if (check_result_value(j_response, G_OK)) {
            if (json_object_get(j_response, "response") != NULL) {
              j_return = json_pack("{sisO}", "result", G_OK, "register", json_object_get(j_response, "response"));
            } else {
              j_return = json_pack("{si}", "result", G_OK);
            }
          } else if (j_response != NULL && !check_result_value(j_response, G_ERROR)) {
            if (json_object_get(j_response, "response") != NULL) {
              j_return = json_pack("{sIsO}", "result", json_integer_value(json_object_get(j_response, "result")), "register", json_object_get(j_response, "response"));
            } else {
              j_return = json_pack("{sI}", "result", json_integer_value(json_object_get(j_response, "result")));
            }
          } else {
            y_log_message(Y_LOG_LEVEL_ERROR, "auth_register_user_scheme - Error user_auth_scheme_module_register");
            j_return = json_pack("{si}", "result", G_ERROR);
          }
          json_decref(j_response);
        } else {
          j_return = json_pack("{si}", "result", G_ERROR_UNAUTHORIZED);
        }
      } else {
        j_return = json_pack("{si}", "result", G_ERROR_UNAUTHORIZED);
      }
    } else {
      j_return = json_pack("{si}", "result", G_ERROR_PARAM);
    }
  } else if (res != G_ERROR_NOT_FOUND) {
    y_log_message(Y_LOG_LEVEL_ERROR, "auth_register_user_scheme - Error user_has_scheme");
    j_return = json_pack("{si}", "result", G_ERROR);
  } else {
    j_return = json_pack("{si}", "result", G_ERROR_UNAUTHORIZED);
  }
  return j_return;
}

json_t * auth_register_get_user_scheme(struct config_elements * config, const char * scheme_type, const char * scheme_name, const char * username, const struct _u_request * request) {
  struct _user_auth_scheme_module_instance * scheme_instance;
  json_t * j_return = NULL, * j_response = NULL;
  int res;
  
  if ((res = user_has_scheme(config, username, scheme_name)) == G_OK) {
    scheme_instance = get_user_auth_scheme_module_instance(config, scheme_name);
    if (scheme_instance != NULL && 0 == o_strcmp(scheme_type, scheme_instance->module->name) && scheme_instance->enabled) {
      j_response = scheme_instance->module->user_auth_scheme_module_register_get(config->config_m, request, username, scheme_instance->cls);
      if (check_result_value(j_response, G_OK)) {
        if (json_object_get(j_response, "response") != NULL) {
          j_return = json_pack("{sisO}", "result", G_OK, "register", json_object_get(j_response, "response"));
        } else {
          j_return = json_pack("{si}", "result", G_OK);
        }
      } else if (!check_result_value(j_response, G_ERROR)) {
        j_return = json_incref(j_response);
      } else {
        y_log_message(Y_LOG_LEVEL_ERROR, "auth_register_get_user_scheme - Error user_auth_scheme_module_register_get");
        j_return = json_pack("{si}", "result", G_ERROR);
      }
      json_decref(j_response);
    } else {
      j_return = json_pack("{si}", "result", G_ERROR_UNAUTHORIZED);
    }
  } else if (res != G_ERROR_NOT_FOUND) {
    y_log_message(Y_LOG_LEVEL_ERROR, "auth_register_get_user_scheme - Error user_has_scheme");
    j_return = json_pack("{si}", "result", G_ERROR);
  } else {
    j_return = json_pack("{si}", "result", G_ERROR_UNAUTHORIZED);
  }
  return j_return;
}

int user_has_scope(json_t * j_user, const char * scope) {
  json_t * j_element;
  size_t index;
  
  json_array_foreach(json_object_get(j_user, "scope"), index, j_element) {
    if (0 == o_strcmp(scope, json_string_value(j_element))) {
      return 1;
    }
  }
  return 0;
}

int user_has_scheme(struct config_elements * config, const char * username, const char * scheme_name) {
  json_t * j_user, * j_element = NULL, * j_group = NULL, * j_scheme = NULL, * j_scope = NULL;
  size_t index = 0, index_s = 0;
  const char * group = NULL;
  int ret = G_ERROR_NOT_FOUND;
  
  j_user = get_user(config, username, NULL);
  if (check_result_value(j_user, G_OK)) {
    json_array_foreach(json_object_get(json_object_get(j_user, "user"), "scope"), index, j_element) {
      j_scope = get_scope(config, json_string_value(j_element));
      if (check_result_value(j_scope, G_OK)) {
        json_object_foreach(json_object_get(json_object_get(j_scope, "scope"), "scheme"), group, j_group) {
          json_array_foreach(j_group, index_s, j_scheme) {
            if (0 == o_strcmp(json_string_value(json_object_get(j_scheme, "scheme_name")), scheme_name)) {
              ret = G_OK;
            }
          }
        }
      } else {
        y_log_message(Y_LOG_LEVEL_ERROR, "user_has_scheme - Error get_scope '%s'", json_string_value(j_element));
      }
      json_decref(j_scope);
    }
  } else if (!check_result_value(j_user, G_ERROR_NOT_FOUND)) {
    y_log_message(Y_LOG_LEVEL_ERROR, "user_has_scheme - Error get_user");
    ret = G_ERROR;
  }
  json_decref(j_user);
  return ret;
}

json_t * get_user(struct config_elements * config, const char * username, const char * source) {
  int found = 0, result;
  json_t * j_return = NULL, * j_user, * j_module_list, * j_module;
  struct _user_module_instance * user_module;
  struct _user_middleware_module_instance * user_middleware_module;
  size_t index, i;
  
  if (!o_strlen(username)) {
    j_return = json_pack("{si}", "result", G_ERROR_PARAM);
  } else if (source != NULL) {
    user_module = get_user_module_instance(config, source);
    if (user_module != NULL) {
      j_user = user_module->module->user_module_get(config->config_m, username, user_module->cls);
      if (check_result_value(j_user, G_OK)) {
        result = G_OK;
        for (i=0; i<pointer_list_size(config->user_middleware_module_instance_list); i++) {
          user_middleware_module = (struct _user_middleware_module_instance *)pointer_list_get_at(config->user_middleware_module_instance_list, i);
          if (user_middleware_module != NULL && user_middleware_module->enabled) {
            if ((result = user_middleware_module->module->user_middleware_module_get(config->config_m, username, json_object_get(j_user, "user"), user_middleware_module->cls)) != G_OK) {
              y_log_message(Y_LOG_LEVEL_ERROR, "get_user - Error user_middleware_module_get at index %zu for user %s", i, username);
              break;
            }
          } else {
            y_log_message(Y_LOG_LEVEL_ERROR, "get_user - Error pointer_list_get_at for user_middleware module at index %zu", i);
          }
        }
        if (result == G_OK) {
          json_object_set_new(json_object_get(j_user, "user"), "source", json_string(source));
          j_return = json_incref(j_user);
        } else {
          j_return = json_pack("{si}", "result", result);
        }
      } else if (check_result_value(j_user, G_ERROR_NOT_FOUND)) {
        j_return = json_pack("{si}", "result", G_ERROR_NOT_FOUND);
      } else {
        y_log_message(Y_LOG_LEVEL_ERROR, "get_user - Error, user_module_get for module %s", user_module->name);
        j_return = json_pack("{si}", "result", G_ERROR);
      }
      json_decref(j_user);
    } else {
      j_return = json_pack("{si}", "result", G_ERROR_NOT_FOUND);
    }
  } else {
    j_module_list = get_user_module_list(config);
    if (check_result_value(j_module_list, G_OK)) {
      json_array_foreach(json_object_get(j_module_list, "module"), index, j_module) {
        if (!found) {
          user_module = get_user_module_instance(config, json_string_value(json_object_get(j_module, "name")));
          if (user_module != NULL) {
            if (user_module->enabled) {
              j_user = user_module->module->user_module_get(config->config_m, username, user_module->cls);
              if (check_result_value(j_user, G_OK)) {
                found = 1;
                result = G_OK;
                for (i=0; i<pointer_list_size(config->user_middleware_module_instance_list); i++) {
                  user_middleware_module = (struct _user_middleware_module_instance *)pointer_list_get_at(config->user_middleware_module_instance_list, i);
                  if (user_middleware_module != NULL && user_middleware_module->enabled) {
                    if ((result = user_middleware_module->module->user_middleware_module_get(config->config_m, username, json_object_get(j_user, "user"), user_middleware_module->cls)) != G_OK) {
                      y_log_message(Y_LOG_LEVEL_ERROR, "get_user - Error user_middleware_module_get at index %zu for user %s", i, username);
                      break;
                    }
                  } else {
                    y_log_message(Y_LOG_LEVEL_ERROR, "get_user - Error pointer_list_get_at for user_middleware module at index %zu", i);
                  }
                }
                if (result == G_OK) {
                  json_object_set_new(json_object_get(j_user, "user"), "source", json_string(user_module->name));
                  j_return = json_incref(j_user);
                } else {
                  j_return = json_pack("{si}", "result", result);
                }
              } else if (!check_result_value(j_user, G_ERROR_NOT_FOUND)) {
                y_log_message(Y_LOG_LEVEL_ERROR, "get_user - Error, user_module_get for module %s", user_module->name);
              }
              json_decref(j_user);
            }
          } else {
            y_log_message(Y_LOG_LEVEL_ERROR, "get_user - Error, user_module_instance %s is NULL", json_string_value(json_object_get(j_module, "name")));
          }
        }
      }
    } else {
      y_log_message(Y_LOG_LEVEL_ERROR, "get_user - Error get_user_module_list");
      j_return = json_pack("{si}", "result", G_ERROR_NOT_FOUND);
    }
    json_decref(j_module_list);
  }
  if (j_return == NULL) {
    j_return = json_pack("{si}", "result", G_ERROR_NOT_FOUND);
  }
  return j_return;
}

json_t * get_user_profile(struct config_elements * config, const char * username, const char * source) {
  int found = 0, result;
  json_t * j_return = NULL, * j_module_list, * j_module, * j_profile;
  struct _user_module_instance * user_module;
  struct _user_middleware_module_instance * user_middleware_module;
  size_t index, i;
  
  if (source != NULL) {
    user_module = get_user_module_instance(config, source);
    if (user_module != NULL) {
      j_profile = user_module->module->user_module_get_profile(config->config_m, username, user_module->cls);
      if (check_result_value(j_profile, G_OK)) {
        result = G_OK;
        for (i=0; i<pointer_list_size(config->user_middleware_module_instance_list); i++) {
          user_middleware_module = (struct _user_middleware_module_instance *)pointer_list_get_at(config->user_middleware_module_instance_list, i);
          if (user_middleware_module != NULL && user_middleware_module->enabled) {
            if ((result = user_middleware_module->module->user_middleware_module_get_profile(config->config_m, username, json_object_get(j_profile, "user"), user_middleware_module->cls)) != G_OK) {
              y_log_message(Y_LOG_LEVEL_ERROR, "get_user_profile - Error user_middleware_module_get_profile at index %zu for user %s", i, username);
              break;
            }
          } else {
            y_log_message(Y_LOG_LEVEL_ERROR, "get_user_profile - Error pointer_list_get_at for user_middleware module at index %zu", i);
          }
        }
        if (result == G_OK) {
          j_return = json_incref(j_profile);
        } else {
          j_return = json_pack("{si}", "result", result);
        }
      } else if (check_result_value(j_profile, G_ERROR_NOT_FOUND)) {
        j_return = json_pack("{si}", "result", G_ERROR_NOT_FOUND);
      } else {
        y_log_message(Y_LOG_LEVEL_ERROR, "user_get_profile - Error user_module_get_profile");
        j_return = json_pack("{si}", "result", G_ERROR);
      }
      json_decref(j_profile);
    } else {
      j_return = json_pack("{si}", "result", G_ERROR_NOT_FOUND);
    }
  } else {
    j_module_list = get_user_module_list(config);
    if (check_result_value(j_module_list, G_OK)) {
      json_array_foreach(json_object_get(j_module_list, "module"), index, j_module) {
        if (!found) {
          user_module = get_user_module_instance(config, json_string_value(json_object_get(j_module, "name")));
          if (user_module != NULL) {
            if (user_module->enabled) {
              j_profile = user_module->module->user_module_get_profile(config->config_m, username, user_module->cls);
              if (check_result_value(j_profile, G_OK)) {
                result = G_OK;
                for (i=0; i<pointer_list_size(config->user_middleware_module_instance_list); i++) {
                  user_middleware_module = (struct _user_middleware_module_instance *)pointer_list_get_at(config->user_middleware_module_instance_list, i);
                  if (user_middleware_module != NULL && user_middleware_module->enabled) {
                    if ((result = user_middleware_module->module->user_middleware_module_get_profile(config->config_m, username, json_object_get(j_profile, "user"), user_middleware_module->cls)) != G_OK) {
                      y_log_message(Y_LOG_LEVEL_ERROR, "get_user_profile - Error user_middleware_module_get_profile at index %d for user %s", i, username);
                      break;
                    }
                  } else {
                    y_log_message(Y_LOG_LEVEL_ERROR, "get_user_profile - Error pointer_list_get_at for user_middleware module at index %d", i);
                  }
                }
                if (result == G_OK) {
                  j_return = json_incref(j_profile);
                } else {
                  j_return = json_pack("{si}", "result", result);
                }
                found = 1;
              }
              json_decref(j_profile);
            }
          } else {
            y_log_message(Y_LOG_LEVEL_ERROR, "get_user_profile - Error, user_module_instance %s is NULL", json_string_value(json_object_get(j_module, "name")));
          }
        }
      }
    } else {
      y_log_message(Y_LOG_LEVEL_ERROR, "get_user_profile - Error get_user_module_list");
      j_return = json_pack("{si}", "result", G_ERROR);
    }
    json_decref(j_module_list);
  }
  if (j_return == NULL) {
    j_return = json_pack("{si}", "result", G_ERROR_NOT_FOUND);
  }
  return j_return;
}

json_t * get_user_list(struct config_elements * config, const char * pattern, size_t offset, size_t limit, const char * source) {
  json_t * j_return, * j_module_list, * j_module, * j_element, * j_result;
  struct _user_module_instance * user_module;
  struct _user_middleware_module_instance * user_middleware_module;
  size_t cur_offset, cur_limit, count_total, index, index_u, i;
  int result;
  
  if (source != NULL) {
    user_module = get_user_module_instance(config, source);
    if (user_module != NULL && user_module->enabled) {
      j_result = user_module->module->user_module_get_list(config->config_m, pattern, offset, limit, user_module->cls);
      if (check_result_value(j_result, G_OK)) {
        json_array_foreach(json_object_get(j_result, "list"), index, j_element) {
          json_object_set_new(j_element, "source", json_string(source));
        }
        j_return = json_pack("{sisO}", "result", G_OK, "user", json_object_get(j_result, "list"));
      } else {
        y_log_message(Y_LOG_LEVEL_ERROR, "get_user_list - Error user_module_get_list");
        j_return = json_pack("{si}", "result", G_ERROR);
      }
      json_decref(j_result);
    } else if (user_module != NULL && !user_module->enabled) {
      j_return = json_pack("{si}", "result", G_ERROR_PARAM);
    } else {
      y_log_message(Y_LOG_LEVEL_ERROR, "get_user_list - Error get_user_module_instance");
      j_return = json_pack("{si}", "result", G_ERROR);
    }
  } else {
    j_module_list = get_user_module_list(config);
    if (check_result_value(j_module_list, G_OK)) {
      cur_offset = offset;
      cur_limit = limit;
      j_return = json_pack("{sis[]}", "result", G_OK, "user");
      if (j_return != NULL) {
        json_array_foreach(json_object_get(j_module_list, "module"), index, j_module) {
          if (cur_limit) {
            user_module = get_user_module_instance(config, json_string_value(json_object_get(j_module, "name")));
            if (user_module != NULL && user_module->enabled) {
              if ((count_total = user_module->module->user_module_count_total(config->config_m, pattern, user_module->cls)) > cur_offset && cur_limit) {
                j_result = user_module->module->user_module_get_list(config->config_m, pattern, cur_offset, cur_limit, user_module->cls);
                if (check_result_value(j_result, G_OK)) {
                  json_array_foreach(json_object_get(j_result, "list"), index_u, j_element) {
                    json_object_set_new(j_element, "source", json_string(user_module->name));
                  }
                  cur_offset = 0;
                  if (cur_limit > json_array_size(json_object_get(j_result, "list"))) {
                    cur_limit -= json_array_size(json_object_get(j_result, "list"));
                  } else {
                    cur_limit = 0;
                  }
                  json_array_extend(json_object_get(j_return, "user"), json_object_get(j_result, "list"));
                } else {
                  y_log_message(Y_LOG_LEVEL_ERROR, "get_user_list - Error user_module_get_list for module %s", json_string_value(json_object_get(j_module, "name")));
                }
                json_decref(j_result);
              } else {
                cur_offset -= count_total;
              }
            } else if (user_module == NULL) {
              y_log_message(Y_LOG_LEVEL_ERROR, "get_user_list - Error, user_module_instance %s is NULL", json_string_value(json_object_get(j_module, "name")));
            }
          }
        }
      } else {
        y_log_message(Y_LOG_LEVEL_ERROR, "get_user_list - Error allocating resources for j_return");
        j_return = json_pack("{si}", "result", G_ERROR);
      }
    } else {
      y_log_message(Y_LOG_LEVEL_ERROR, "get_user_list - Error get_user_module_list");
      j_return = json_pack("{si}", "result", G_ERROR);
    }
    json_decref(j_module_list);
  }
  if (check_result_value(j_return, G_OK)) {
    result = G_OK;
    for (i=0; i<pointer_list_size(config->user_middleware_module_instance_list); i++) {
      user_middleware_module = (struct _user_middleware_module_instance *)pointer_list_get_at(config->user_middleware_module_instance_list, i);
      if (user_middleware_module != NULL && user_middleware_module->enabled) {
        if ((result = user_middleware_module->module->user_middleware_module_get_list(config->config_m, json_object_get(j_return, "user"), user_middleware_module->cls)) != G_OK) {
          y_log_message(Y_LOG_LEVEL_ERROR, "get_user_list - Error user_middleware_module_get_list at index %zu", i);
          json_decref(j_return);
          j_return = json_pack("{si}", "result", result);
          break;
        }
      } else {
        y_log_message(Y_LOG_LEVEL_ERROR, "get_user_list - Error pointer_list_get_at for user_middleware module at index %zu", i);
      }
    }
  }
  return j_return;
}

json_t * is_user_valid(struct config_elements * config, const char * username, json_t * j_user, int add, const char * source) {
  int found = 0;
  json_t * j_return = NULL, * j_error_list, * j_module_list, * j_module;
  struct _user_module_instance * user_module;
  size_t index;
  
  if (source != NULL) {
    user_module = get_user_module_instance(config, source);
    if (user_module != NULL && user_module->enabled && !user_module->readonly) {
      j_error_list = user_module->module->user_module_is_valid(config->config_m, username, j_user, add?GLEWLWYD_IS_VALID_MODE_ADD:GLEWLWYD_IS_VALID_MODE_UPDATE, user_module->cls);
      if (check_result_value(j_error_list, G_ERROR_PARAM)) {
        j_return = json_incref(j_error_list);
      } else if (check_result_value(j_error_list, G_OK)) {
        j_return = json_pack("{si}", "result", G_OK);
      } else {
        y_log_message(Y_LOG_LEVEL_ERROR, "is_user_valid - Error user_module_is_valid");
        j_return = json_pack("{si}", "result", G_ERROR);
      }
      json_decref(j_error_list);
    } else if (user_module != NULL && user_module->readonly) {
      j_return = json_pack("{sis[s]}", "result", G_ERROR_PARAM, "error", "module is read-only");
    } else if (user_module != NULL && !user_module->enabled) {
      j_return = json_pack("{sis[s]}", "result", G_ERROR_PARAM, "error", "module is unavailable");
    } else {
      y_log_message(Y_LOG_LEVEL_ERROR, "is_user_valid - Error get_user_module_instance");
      j_return = json_pack("{si}", "result", G_ERROR_NOT_FOUND);
    }
  } else if (add) {
    j_module_list = get_user_module_list(config);
    if (check_result_value(j_module_list, G_OK)) {
      json_array_foreach(json_object_get(j_module_list, "module"), index, j_module) {
        if (!found) {
          user_module = get_user_module_instance(config, json_string_value(json_object_get(j_module, "name")));
          if (user_module != NULL && user_module->enabled && !user_module->readonly) {
            found = 1;
            j_error_list = user_module->module->user_module_is_valid(config->config_m, username, j_user, add?GLEWLWYD_IS_VALID_MODE_ADD:GLEWLWYD_IS_VALID_MODE_UPDATE, user_module->cls);
            if (check_result_value(j_error_list, G_ERROR_PARAM)) {
              j_return = json_incref(j_error_list);
            } else if (check_result_value(j_error_list, G_OK)) {
              j_return = json_pack("{si}", "result", G_OK);
            } else {
              y_log_message(Y_LOG_LEVEL_ERROR, "is_user_valid - Error user_module_is_valid");
              j_return = json_pack("{si}", "result", G_ERROR);
            }
            json_decref(j_error_list);
          } else if (user_module == NULL) {
            y_log_message(Y_LOG_LEVEL_ERROR, "is_user_valid - Error, user_module_instance %s is NULL", json_string_value(json_object_get(j_module, "name")));
          }
        }
      }
    } else {
      y_log_message(Y_LOG_LEVEL_ERROR, "is_user_valid - Error get_user_module_list");
      j_return = json_pack("{si}", "result", G_ERROR);
    }
    json_decref(j_module_list);
    if (j_return == NULL) {
      j_return = json_pack("{sis[s]}", "result", G_ERROR_PARAM, "user", "no writeable source");
    }
  } else {
    j_return = json_pack("{sis[s]}", "result", G_ERROR_PARAM, "user", "source parameter is mandatory");
  }
  return j_return;
}

int add_user(struct config_elements * config, json_t * j_user, const char * source) {
  int found = 0, result = G_OK, ret;
  json_t * j_module_list, * j_module;
  struct _user_module_instance * user_module;
  struct _user_middleware_module_instance * user_middleware_module;
  size_t index, i;
  
  if (source != NULL) {
    user_module = get_user_module_instance(config, source);
    if (user_module != NULL && user_module->enabled && !user_module->readonly) {
      for (i=0; i<pointer_list_size(config->user_middleware_module_instance_list); i++) {
        user_middleware_module = (struct _user_middleware_module_instance *)pointer_list_get_at(config->user_middleware_module_instance_list, i);
        if (user_middleware_module != NULL && user_middleware_module->enabled) {
          if ((result = user_middleware_module->module->user_middleware_module_update(config->config_m, json_string_value(json_object_get(j_user, "username")), j_user, user_middleware_module->cls)) != G_OK) {
            y_log_message(Y_LOG_LEVEL_ERROR, "add_user - Error user_middleware_module_get_list at index %zu for user %s", i, json_string_value(json_object_get(j_user, "username")));
            break;
          }
        } else {
          y_log_message(Y_LOG_LEVEL_ERROR, "add_user - Error pointer_list_get_at for user_middleware module at index %zu", i);
        }
      }
      if (result == G_OK) {
        result = user_module->module->user_module_add(config->config_m, j_user, user_module->cls);
        if (result == G_OK) {
          ret = G_OK;
        } else {
          y_log_message(Y_LOG_LEVEL_ERROR, "add_user - Error user_module_add");
          ret = result;
        }
      }
    } else if (user_module != NULL && (user_module->readonly || !user_module->enabled)) {
      y_log_message(Y_LOG_LEVEL_ERROR, "add_user - Error module %s not allowed", user_module->name);
      ret = G_ERROR_PARAM;
    } else {
      y_log_message(Y_LOG_LEVEL_ERROR, "add_user - Error get_user_module_instance");
      ret = G_ERROR;
    }
  } else {
    j_module_list = get_user_module_list(config);
    if (check_result_value(j_module_list, G_OK)) {
      json_array_foreach(json_object_get(j_module_list, "module"), index, j_module) {
        if (!found) {
          user_module = get_user_module_instance(config, json_string_value(json_object_get(j_module, "name")));
          if (user_module != NULL && user_module->enabled && !user_module->readonly) {
            for (i=0; i<pointer_list_size(config->user_middleware_module_instance_list); i++) {
              user_middleware_module = (struct _user_middleware_module_instance *)pointer_list_get_at(config->user_middleware_module_instance_list, i);
              if (user_middleware_module != NULL && user_middleware_module->enabled) {
                if ((result = user_middleware_module->module->user_middleware_module_update(config->config_m, json_string_value(json_object_get(j_user, "username")), j_user, user_middleware_module->cls)) != G_OK) {
                  y_log_message(Y_LOG_LEVEL_ERROR, "add_user - Error user_middleware_module_get_list at index %zu for user %s", i, json_string_value(json_object_get(j_user, "username")));
                  break;
                }
              } else {
                y_log_message(Y_LOG_LEVEL_ERROR, "add_user - Error pointer_list_get_at for user_middleware module at index %zu", i);
              }
            }
            found = 1;
            if (result == G_OK) {
              result = user_module->module->user_module_add(config->config_m, j_user, user_module->cls);
              if (result == G_OK) {
                ret = G_OK;
              } else {
                y_log_message(Y_LOG_LEVEL_ERROR, "add_user - Error user_module_add");
                ret = result;
              }
            }
          } else if (user_module == NULL) {
            y_log_message(Y_LOG_LEVEL_ERROR, "add_user - Error, user_module_instance %s is NULL", json_string_value(json_object_get(j_module, "name")));
          }
        }
      }
    } else {
      y_log_message(Y_LOG_LEVEL_ERROR, "add_user - Error get_user_module_list");
      ret = G_ERROR;
    }
    json_decref(j_module_list);
    if (!found) {
      ret = G_ERROR_NOT_FOUND;
    }
  }
  return ret;
}

int set_user(struct config_elements * config, const char * username, json_t * j_user, const char * source) {
  int ret, result = G_OK;
  struct _user_module_instance * user_module;
  struct _user_middleware_module_instance * user_middleware_module;
  json_t * j_cur_user;
  size_t i;
  
  if (source != NULL) {
    user_module = get_user_module_instance(config, source);
    if (user_module != NULL && user_module->enabled && !user_module->readonly) {
      for (i=0; i<pointer_list_size(config->user_middleware_module_instance_list); i++) {
        user_middleware_module = (struct _user_middleware_module_instance *)pointer_list_get_at(config->user_middleware_module_instance_list, i);
        if (user_middleware_module != NULL && user_middleware_module->enabled) {
          if ((result = user_middleware_module->module->user_middleware_module_update(config->config_m, username, j_user, user_middleware_module->cls)) != G_OK) {
            y_log_message(Y_LOG_LEVEL_ERROR, "set_user - Error user_middleware_module_update at index %zu for user %s", i, username);
            break;
          }
        } else {
          y_log_message(Y_LOG_LEVEL_ERROR, "set_user - Error pointer_list_get_at for user_middleware module at index %zu", i);
        }
      }
      if (result == G_OK) {
        j_cur_user = user_module->module->user_module_get(config->config_m, username, user_module->cls);
        if (check_result_value(j_cur_user, G_OK)) {
          ret = user_module->module->user_module_update(config->config_m, username, j_user, user_module->cls);
          if (ret != G_OK) {
            y_log_message(Y_LOG_LEVEL_ERROR, "set_user - Error user_module_update");
          }
        } else if (check_result_value(j_cur_user, G_ERROR_NOT_FOUND)) {
          ret = G_ERROR_NOT_FOUND;
        } else {
          y_log_message(Y_LOG_LEVEL_ERROR, "set_user - Error user_module_get");
          ret = G_ERROR;
        }
        json_decref(j_cur_user);
      }
    } else if (user_module != NULL && (user_module->readonly || !user_module->enabled)) {
      ret = G_ERROR_PARAM;
    } else {
      y_log_message(Y_LOG_LEVEL_ERROR, "set_user - Error get_user_module_instance");
      ret = G_ERROR;
    }
  } else {
    ret = G_ERROR_PARAM;
  }
  return ret;
}

int delete_user(struct config_elements * config, const char * username, const char * source) {
  int ret;
  struct _user_module_instance * user_module;
  struct _user_middleware_module_instance * user_middleware_module;
  struct _user_auth_scheme_module_instance * scheme_module;
  json_t * j_cur_user;
  int result;
  size_t i;
  
  if (source != NULL) {
    user_module = get_user_module_instance(config, source);
    if (user_module != NULL && user_module->enabled && !user_module->readonly) {
      j_cur_user = user_module->module->user_module_get(config->config_m, username, user_module->cls);
      if (check_result_value(j_cur_user, G_OK)) {
        for (i=0; i<pointer_list_size(config->user_middleware_module_instance_list); i++) {
          user_middleware_module = (struct _user_middleware_module_instance *)pointer_list_get_at(config->user_middleware_module_instance_list, i);
          if (user_middleware_module != NULL && user_middleware_module->enabled) {
            if ((result = user_middleware_module->module->user_middleware_module_delete(config->config_m, username, j_cur_user, user_middleware_module->cls)) != G_OK) {
              y_log_message(Y_LOG_LEVEL_ERROR, "delete_user - Error user_middleware_module_delete at index %zu for user %s", i, username);
              break;
            }
          } else {
            y_log_message(Y_LOG_LEVEL_ERROR, "delete_user - Error pointer_list_get_at for user_middleware module at index %zu", i);
          }
        }
        result = user_module->module->user_module_delete(config->config_m, username, user_module->cls);
        if (result == G_OK) {
          ret = G_OK;
        } else {
          y_log_message(Y_LOG_LEVEL_ERROR, "delete_user - Error user_module_delete");
          ret = result;
        }
      } else if (check_result_value(j_cur_user, G_ERROR_NOT_FOUND)) {
        ret = G_ERROR_NOT_FOUND;
      } else {
        y_log_message(Y_LOG_LEVEL_ERROR, "delete_user - Error user_module_get");
        ret = G_ERROR;
      }
      if (ret == G_OK) {
        for (i = 0; i < pointer_list_size(config->user_auth_scheme_module_instance_list); i++) {
          scheme_module = pointer_list_get_at(config->user_auth_scheme_module_instance_list, i);
          if (scheme_module != NULL && scheme_module->enabled) {
            if ((ret = scheme_module->module->user_auth_scheme_module_deregister(config->config_m, username, scheme_module->cls)) != G_OK) {
              y_log_message(Y_LOG_LEVEL_ERROR, "delete_user - Error user_auth_scheme_module_deregister for scheme %s", scheme_module->name);
              break;
            }
          }
        }
      }
      json_decref(j_cur_user);
    } else if (user_module != NULL && (user_module->readonly || !user_module->enabled)) {
      ret = G_ERROR_PARAM;
    } else {
      y_log_message(Y_LOG_LEVEL_ERROR, "delete_user - Error get_user_module_instance");
      ret = G_ERROR;
    }
  } else {
    ret = G_ERROR_PARAM;
  }
  return ret;
}

json_t * user_get_profile(struct config_elements * config, const char * username) {
  json_t * j_user = get_user(config, username, NULL), * j_return, * j_profile;
  struct _user_module_instance * user_module;

  if (check_result_value(j_user, G_OK)) {
    user_module = get_user_module_instance(config, json_string_value(json_object_get(json_object_get(j_user, "user"), "source")));
    if (user_module != NULL && user_module->enabled) {
      j_profile = user_module->module->user_module_get_profile(config->config_m, username, user_module->cls);
      if (check_result_value(j_profile, G_OK)) {
        j_return = json_pack("{sisO}", "result", G_OK, "profile", j_profile);
      } else {
        y_log_message(Y_LOG_LEVEL_ERROR, "user_get_profile - Error user_module_get_profile");
        j_return = json_pack("{si}", "result", G_ERROR);
      }
      json_decref(j_profile);
    } else {
      y_log_message(Y_LOG_LEVEL_ERROR, "user_get_profile - Error get_user_module_instance");
      j_return = json_pack("{si}", "result", G_ERROR);
    }
  } else if (check_result_value(j_user, G_ERROR_NOT_FOUND)) {
    j_return = json_pack("{si}", "result", G_ERROR_NOT_FOUND);
  } else {
    y_log_message(Y_LOG_LEVEL_ERROR, "user_get_profile - Error get_user");
    j_return = json_pack("{si}", "result", G_ERROR);
  }
  json_decref(j_user);
  return j_return;
}

json_t * user_set_profile(struct config_elements * config, const char * username, json_t * j_profile) {
  json_t * j_user = get_user(config, username, NULL), * j_return;
  struct _user_module_instance * user_module;

  if (check_result_value(j_user, G_OK)) {
    user_module = get_user_module_instance(config, json_string_value(json_object_get(json_object_get(j_user, "user"), "source")));
    if (user_module != NULL && user_module->enabled && !user_module->readonly) {
      j_return = json_pack("{si}", "result", user_module->module->user_module_update_profile(config->config_m, username, j_profile, user_module->cls));
    } else if (user_module != NULL && (user_module->readonly || !user_module->enabled)) {
      j_return = json_pack("{sis[s]}", "result", G_ERROR_PARAM, "error", "profile update is not allowed");
    } else {
      y_log_message(Y_LOG_LEVEL_ERROR, "user_set_profile - Error get_user_module_instance");
      j_return = json_pack("{si}", "result", G_ERROR);
    }
  } else if (check_result_value(j_user, G_ERROR_NOT_FOUND)) {
    j_return = json_pack("{si}", "result", G_ERROR_NOT_FOUND);
  } else {
    y_log_message(Y_LOG_LEVEL_ERROR, "user_set_profile - Error get_user");
    j_return = json_pack("{si}", "result", G_ERROR);
  }
  json_decref(j_user);
  return j_return;
}

int user_delete_profile(struct config_elements * config, const char * username) {
  json_t * j_user = get_user(config, username, NULL);
  struct _user_module_instance * user_module;
  struct _user_auth_scheme_module_instance * scheme_module;
  int ret;
  size_t i;

  if (check_result_value(j_user, G_OK)) {
    user_module = get_user_module_instance(config, json_string_value(json_object_get(json_object_get(j_user, "user"), "source")));
    if (config->delete_profile & GLEWLWYD_PROFILE_DELETE_AUTHORIZED && user_module != NULL && user_module->enabled && !user_module->readonly) {
      ret = G_OK;
      if (config->delete_profile & GLEWLWYD_PROFILE_DELETE_DISABLE_PROFILE) {
        json_object_set(json_object_get(j_user, "user"), "enabled", json_false());
        if ((ret = user_module->module->user_module_update(config->config_m, username, json_object_get(j_user, "user"), user_module->cls)) != G_OK) {
          y_log_message(Y_LOG_LEVEL_ERROR, "user_delete_profile - Error user_module_update_profile");
        }
      } else {
        if ((ret = user_module->module->user_module_delete(config->config_m, username, user_module->cls)) != G_OK) {
          y_log_message(Y_LOG_LEVEL_ERROR, "user_delete_profile - Error user_module_delete");
        }
      }
      if (ret == G_OK && !(config->delete_profile & GLEWLWYD_PROFILE_DELETE_DISABLE_PROFILE)) {
        for (i = 0; i < pointer_list_size(config->user_auth_scheme_module_instance_list); i++) {
          scheme_module = pointer_list_get_at(config->user_auth_scheme_module_instance_list, i);
          if (scheme_module != NULL && scheme_module->enabled) {
            if ((ret = scheme_module->module->user_auth_scheme_module_deregister(config->config_m, username, scheme_module->cls)) != G_OK) {
              y_log_message(Y_LOG_LEVEL_ERROR, "user_delete_profile - Error user_auth_scheme_module_deregister for scheme %s", scheme_module->name);
              break;
            }
          }
        }
      }
    } else if (!(config->delete_profile & GLEWLWYD_PROFILE_DELETE_AUTHORIZED) || (user_module != NULL && user_module->readonly)) {
      ret = G_ERROR_UNAUTHORIZED;
    } else {
      ret = G_ERROR;
    }
  } else if (check_result_value(j_user, G_ERROR_NOT_FOUND)) {
    ret = G_ERROR_NOT_FOUND;
  } else {
    y_log_message(Y_LOG_LEVEL_ERROR, "user_delete_profile - Error get_user");
    ret = G_ERROR;
  }
  json_decref(j_user);
  return ret;
}

int user_update_password(struct config_elements * config, const char * username, const char * old_password, const char ** new_passwords, size_t new_passwords_len) {
  json_t * j_user = get_user(config, username, NULL);
  struct _user_module_instance * user_module;
  int ret;

  if (check_result_value(j_user, G_OK)) {
    user_module = get_user_module_instance(config, json_string_value(json_object_get(json_object_get(j_user, "user"), "source")));
    if (user_module != NULL && user_module->enabled && !user_module->readonly) {
      if ((ret = user_module->module->user_module_check_password(config->config_m, username, old_password, user_module->cls)) == G_OK) {
        ret = user_module->module->user_module_update_password(config->config_m, username, new_passwords, new_passwords_len, user_module->cls);
      } else if (ret == G_ERROR_UNAUTHORIZED) {
        ret = G_ERROR_PARAM;
      } else {
        y_log_message(Y_LOG_LEVEL_ERROR, "user_set_profile - Error user_module_check_password");
        ret = G_ERROR;
      }
    } else if (user_module != NULL && (user_module->readonly || !user_module->enabled)) {
      ret = G_ERROR_PARAM;
    } else {
      y_log_message(Y_LOG_LEVEL_ERROR, "user_set_profile - Error get_user_module_instance");
      ret = G_ERROR;
    }
  } else if (check_result_value(j_user, G_ERROR_NOT_FOUND)) {
    ret = G_ERROR_NOT_FOUND;
  } else {
    y_log_message(Y_LOG_LEVEL_ERROR, "user_set_profile - Error get_user");
    ret = G_ERROR;
  }
  json_decref(j_user);
  return ret;
}

int user_set_password(struct config_elements * config, const char * username, const char ** new_passwords, size_t new_passwords_len) {
  json_t * j_user = get_user(config, username, NULL);
  struct _user_module_instance * user_module;
  int ret;

  if (check_result_value(j_user, G_OK)) {
    user_module = get_user_module_instance(config, json_string_value(json_object_get(json_object_get(j_user, "user"), "source")));
    if (user_module != NULL && user_module->enabled && !user_module->readonly) {
      ret = user_module->module->user_module_update_password(config->config_m, username, new_passwords, new_passwords_len, user_module->cls);
    } else if (user_module != NULL && (user_module->readonly || !user_module->enabled)) {
      ret = G_ERROR_PARAM;
    } else {
      y_log_message(Y_LOG_LEVEL_ERROR, "user_set_profile - Error get_user_module_instance");
      ret = G_ERROR;
    }
  } else if (check_result_value(j_user, G_ERROR_NOT_FOUND)) {
    ret = G_ERROR_NOT_FOUND;
  } else {
    y_log_message(Y_LOG_LEVEL_ERROR, "user_set_profile - Error get_user");
    ret = G_ERROR;
  }
  json_decref(j_user);
  return ret;
}

json_t * glewlwyd_module_callback_get_user(struct config_module * config, const char * username) {
  return get_user(config->glewlwyd_config, username, NULL);
}

int glewlwyd_module_callback_set_user(struct config_module * config, const char * username, json_t * j_user_data) {
  json_t * j_user;
  int ret;
  
  j_user = get_user(config->glewlwyd_config, username, NULL);
  if (check_result_value(j_user, G_OK)) {
    ret = set_user(config->glewlwyd_config, username, j_user_data, json_string_value(json_object_get(json_object_get(j_user, "user"), "source")));
  } else {
    y_log_message(Y_LOG_LEVEL_ERROR, "glewlwyd_callback_set_user - Error get_user");
    ret = G_ERROR;
  }
  json_decref(j_user);
  return ret;
}

int glewlwyd_module_callback_check_user_password(struct config_module * config, const char * username, const char * password) {
  int ret;
  json_t * j_result;
  
  j_result = auth_check_user_credentials(config->glewlwyd_config, username, password);
  if (json_is_integer(json_object_get(j_result, "result"))) {
    ret = json_integer_value(json_object_get(j_result, "result"));
  } else {
    y_log_message(Y_LOG_LEVEL_ERROR, "glewlwyd_module_callback_check_user_password - Error auth_check_user_credentials");
    ret = G_ERROR;
  }
  json_decref(j_result);
  return ret;
}

json_t * glewlwyd_module_callback_check_user_session(struct config_module * config, const struct _u_request * request, const char * username) {
  char * session_uid = get_session_id(config->glewlwyd_config, request);
  json_t * j_return, * j_result;
  if (session_uid != NULL) {
    j_result = get_current_user_for_session(config->glewlwyd_config, session_uid);
    if (check_result_value(j_result, G_OK)) {
      if (0 == o_strcmp(username, json_string_value(json_object_get(json_object_get(j_result, "user"), "username")))) {
        j_return = json_incref(j_result);
      } else {
        j_return = json_pack("{si}", "result", G_ERROR_UNAUTHORIZED);
      }
    } else if (!check_result_value(j_result, G_ERROR_NOT_FOUND)) {
      y_log_message(Y_LOG_LEVEL_ERROR, "glewlwyd_module_callback_check_user_password - Error get_current_user_for_session");
      j_return = json_pack("{si}", "result", G_ERROR);
    } else {
      j_return = json_pack("{si}", "result", G_ERROR_UNAUTHORIZED);
    }
    json_decref(j_result);
  } else {
    j_return = json_pack("{si}", "result", G_ERROR_UNAUTHORIZED);
  }
  o_free(session_uid);
  return j_return;
}
