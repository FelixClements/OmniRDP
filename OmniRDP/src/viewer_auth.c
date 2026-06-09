#include "viewer_auth.h"

#include <stdlib.h>
#include <string.h>

static int viewer_auth_string_has_value(const char *value) {
  return value && value[0];
}

static char *viewer_auth_strdup_range(const char *start, size_t length) {
  char *result = NULL;

  if (!start)
    return NULL;

  result = (char *)calloc(length + 1, sizeof(char));
  if (!result)
    return NULL;

  for (size_t i = 0; i < length; ++i)
    result[i] = start[i];

  return result;
}

void viewer_auth_credentials_clear(ViewerAuthCredentials *credentials) {
  if (!credentials)
    return;

  free(credentials->username);
  free(credentials->domain);
  free(credentials->password);
  credentials->username = NULL;
  credentials->domain = NULL;
  credentials->password = NULL;
}

int viewer_auth_credentials_usable(const ViewerAuthCredentials *credentials) {
  if (!credentials)
    return 0;

  return viewer_auth_string_has_value(credentials->username) &&
         viewer_auth_string_has_value(credentials->password);
}

int viewer_auth_normalize_domain_user(ViewerAuthCredentials *credentials) {
  const char *separator = NULL;
  char *normalized_domain = NULL;
  char *normalized_user = NULL;

  if (!credentials || !credentials->username || !credentials->domain)
    return 1;

  if (viewer_auth_string_has_value(credentials->domain))
    return 1;

  separator = strchr(credentials->username, '\\');
  if (!separator || (separator == credentials->username) || !separator[1])
    return 1;

  normalized_domain = viewer_auth_strdup_range(
      credentials->username, (size_t)(separator - credentials->username));
  normalized_user = _strdup(separator + 1);
  if (!normalized_domain || !normalized_user) {
    free(normalized_domain);
    free(normalized_user);
    return 0;
  }

  free(credentials->domain);
  free(credentials->username);
  credentials->domain = normalized_domain;
  credentials->username = normalized_user;
  return 1;
}

int viewer_auth_should_defer_backend_credentials(int nla_enabled,
                                                 int automatic) {
  return !nla_enabled && !automatic;
}

ViewerAuthSelection
viewer_auth_select_credentials(const ViewerAuthCredentials *identity,
                               const ViewerAuthCredentials *settings,
                               int nla_enabled) {
  ViewerAuthSelection selection = {0};
  const int identity_usable = viewer_auth_credentials_usable(identity);
  const int settings_usable = viewer_auth_credentials_usable(settings);

  selection.source = "none";

  if (nla_enabled) {
    if (identity_usable) {
      selection.credentials = identity;
      selection.source = "identity";
    } else if (settings_usable) {
      selection.credentials = settings;
      selection.source = "settings-fallback";
    }
  } else {
    if (settings_usable) {
      selection.credentials = settings;
      selection.source = "settings";
    } else if (identity_usable) {
      selection.credentials = identity;
      selection.source = "identity-fallback";
    }
  }

  return selection;
}
