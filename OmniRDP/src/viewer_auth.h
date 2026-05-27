#ifndef VIEWER_AUTH_H
#define VIEWER_AUTH_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  char *username;
  char *domain;
  char *password;
} ViewerAuthCredentials;

typedef struct {
  const ViewerAuthCredentials *credentials;
  const char *source;
} ViewerAuthSelection;

void viewer_auth_credentials_clear(ViewerAuthCredentials *credentials);
int viewer_auth_credentials_usable(const ViewerAuthCredentials *credentials);
int viewer_auth_normalize_domain_user(ViewerAuthCredentials *credentials);
int viewer_auth_should_defer_backend_credentials(int nla_enabled,
                                                 int automatic);
ViewerAuthSelection
viewer_auth_select_credentials(const ViewerAuthCredentials *identity,
                               const ViewerAuthCredentials *settings,
                               int nla_enabled);

#ifdef __cplusplus
}
#endif

#endif /* VIEWER_AUTH_H */
