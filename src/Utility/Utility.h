#ifndef UTILITY_H
#define UTILITY_H

#include <beaker.h>

#ifndef VERSION
#define VERSION "unknown"
#endif

#ifndef GIT_REMOTE
#define GIT_REMOTE "https://git.bwaaa.space/omnisearch"
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
const char *get_title_font(void);

/* user-defined palette, stored as "rrggbb,rrggbb,..." in a cookie */
#define CUSTOM_COLOUR_COUNT 8
typedef struct {
  char bg_main[7];
  char bg_card[7];
  char border[7];
  char text_primary[7];
  char text_secondary[7];
  char text_muted[7];
  char accent[7];
  char text_dim[7];
} CustomTheme;

void get_custom_theme(CustomTheme *out);
void context_set_custom_theme(TemplateContext *ctx, const CustomTheme *theme);
int parse_custom_theme_cookie(const char *value, CustomTheme *out);
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
