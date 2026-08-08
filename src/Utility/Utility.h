#ifndef UTILITY_H
#define UTILITY_H

#include <beaker.h>

#ifndef VERSION
#define VERSION "unknown"
#endif

#ifndef GIT_REMOTE
#define GIT_REMOTE "https://git.bwaaa.monster/omnisearch"
#endif

#define LINK_FIELD_COUNT 3

int hex_to_int(char c);
char *get_theme(const char *default_theme);
void init_themes(const char *static_path);
void get_available_themes(char ***out_themes, int *out_count);
void set_default_locale(const char *locale);
char *get_locale(const char *default_locale);

int is_engine_id_enabled(const char *engine_id);
int get_user_engines(char ***out_ids, int *out_count);
int user_engines_contains(const char *engine_id, char **ids, int count);

int append_string_row(char ****matrix, int **inner_counts, int row_count,
                      const char *const *values, int field_count);
void free_string_matrix(char ***matrix, int *inner_counts, int row_count);

int add_link_to_collection(const char *href, const char *label,
                           const char *class_name, char ****collection,
                           int **inner_counts, int current_count);

int build_pagination(int page, char *(*href_builder)(int page, void *data),
                     void *data, char ****out_matrix, int **out_inner_counts);

#endif
