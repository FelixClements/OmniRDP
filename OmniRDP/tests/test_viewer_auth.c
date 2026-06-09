#include "viewer_auth.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *dup_text(const char *text) { return _strdup(text ? text : ""); }

static ViewerAuthCredentials make_credentials(const char *username,
                                              const char *domain,
                                              const char *password) {
  ViewerAuthCredentials credentials = {0};
  credentials.username = dup_text(username);
  credentials.domain = dup_text(domain);
  credentials.password = dup_text(password);
  return credentials;
}

static int exact_match(const ViewerAuthCredentials *credentials,
                       const char *username, const char *domain,
                       const char *password) {
  if (!credentials || !credentials->username || !credentials->domain ||
      !credentials->password)
    return 0;

  return strcmp(credentials->username, username) == 0 &&
         strcmp(credentials->domain, domain) == 0 &&
         strcmp(credentials->password, password) == 0;
}

int main(void) {
  int ok = 1;
  ViewerAuthCredentials empty_identity = make_credentials("", "", "");
  ViewerAuthCredentials valid_settings =
      make_credentials("localadmin", "vm2", "correct");
  ViewerAuthCredentials valid_identity =
      make_credentials("localadmin", "vm2", "correct");
  ViewerAuthCredentials wrong_settings =
      make_credentials("localadmin", "vm2", "wrong");
  ViewerAuthCredentials settings_without_password =
      make_credentials("localadmin", "vm2", "");
  ViewerAuthCredentials domain_user =
      make_credentials("vm2\\localadmin", "", "correct");
  ViewerAuthSelection selection = {0};

  selection =
      viewer_auth_select_credentials(&empty_identity, &valid_settings, 0);
  if (selection.credentials != &valid_settings ||
      strcmp(selection.source, "settings") != 0)
    ok = 0;

  selection =
      viewer_auth_select_credentials(&valid_identity, &wrong_settings, 1);
  if (selection.credentials != &valid_identity ||
      strcmp(selection.source, "identity") != 0)
    ok = 0;

  selection = viewer_auth_select_credentials(&empty_identity,
                                             &settings_without_password, 0);
  if (selection.credentials || strcmp(selection.source, "none") != 0)
    ok = 0;

  if (!viewer_auth_normalize_domain_user(&domain_user) ||
      !exact_match(&domain_user, "localadmin", "vm2", "correct"))
    ok = 0;

  if (exact_match(&wrong_settings, "localadmin", "vm2", "correct"))
    ok = 0;

  if (exact_match(&valid_settings, "localadmin", "wrongdomain", "correct"))
    ok = 0;

  if (!viewer_auth_should_defer_backend_credentials(0, 0))
    ok = 0;

  if (viewer_auth_should_defer_backend_credentials(1, 0))
    ok = 0;

  if (viewer_auth_should_defer_backend_credentials(0, 1))
    ok = 0;

  viewer_auth_credentials_clear(&empty_identity);
  viewer_auth_credentials_clear(&valid_settings);
  viewer_auth_credentials_clear(&valid_identity);
  viewer_auth_credentials_clear(&wrong_settings);
  viewer_auth_credentials_clear(&settings_without_password);
  viewer_auth_credentials_clear(&domain_user);

  return ok ? 0 : 1;
}
