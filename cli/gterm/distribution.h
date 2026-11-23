#ifndef GTERM_DISTRIBUTION_H
#define GTERM_DISTRIBUTION_H

int gterm_distribution_platform(void);
int gterm_distribution_command(const char *executable, const char *command);
int gterm_verify_update_index(const char *index_path, const char *signature_path);

#endif
