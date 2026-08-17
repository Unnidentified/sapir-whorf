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

typedef struct {
  char ***rows;
  int *field_counts;
  int count;
  int capacity;
} StringMatrix;

int hex_to_int(char c);
char *get_theme(const char *default_theme);
void init_themes(const char *static_path);
void get_available_themes(char ***out_themes, int *out_count);
void set_default_locale(const char *locale);
char *get_locale(const char *default_locale);

int is_engine_id_enabled(const char *engine_id);
int get_user_engines(char ***out_ids, int *out_count);
int user_engines_contains(const char *engine_id, char **ids, int count);
char *get_default_search_engine(void);

void free_string_matrix(char ***matrix, int *inner_counts, int row_count);

void string_matrix_init(StringMatrix *matrix);
int string_matrix_append(StringMatrix *matrix, const char *const *values,
                         int field_count);
void string_matrix_free(StringMatrix *matrix);

int string_matrix_build_pagination(int page,
                                   char *(*href_builder)(int page, void *data),
                                   void *data, StringMatrix *out_matrix);

#endif
