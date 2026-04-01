#ifndef UTILITY_H
#define UTILITY_H

#include <beaker.h>

#define LINK_FIELD_COUNT 3

int hex_to_int(char c);
char *get_theme(const char *default_theme);
char *get_locale(const char *default_locale);

int is_engine_id_enabled(const char *engine_id);
int get_user_engines(char ***out_ids, int *out_count);
int user_engines_contains(const char *engine_id, char **ids, int count);

int add_link_to_collection(const char *href, const char *label,
                           const char *class_name, char ****collection,
                           int **inner_counts, int current_count);

int build_pagination(int page,
                     char *(*href_builder)(int page, void *data), void *data,
                     char ****out_matrix, int **out_inner_counts);

#endif
