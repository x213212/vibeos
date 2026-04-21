#ifndef SSH_CLIENT_H
#define SSH_CLIENT_H

void ssh_client_reset(void);
int ssh_client_set_target(const char *spec, char *out, int out_max);
int ssh_client_set_password(const char *password, char *out, int out_max);
int ssh_client_set_wrp_url(const char *url, char *out, int out_max);
int ssh_client_get_target(char *out, int out_max);
int ssh_client_has_password(void);
int ssh_client_get_wrp_url(char *out, int out_max);
int ssh_client_exec_remote(const char *cmd, char *out, int out_max);

#endif
