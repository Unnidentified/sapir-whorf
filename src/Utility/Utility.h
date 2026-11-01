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

/* percent-encode for a query string; returns the encoded length */
int url_encode(const char *in, char *out, size_t size);
int hex_to_int(char c);
char *get_theme(const char *default_theme);
/* per-user answer settings (cookie, falling back to the server config) */
int answers_mode_ok(const char *value);
int answers_fanout_ok(const char *value);
void get_answers_mode(const char *fallback, char *out, size_t size);
void get_answers_fanout(const char *fallback, char *out, size_t size);
/* J5.5 tier 3: contested providers. available = the server permits them at
   all; allowed = the server permits it AND this browser opted in. */
int opt_in_providers_available(void);
int opt_in_providers_allowed(void);
const char *get_title_font(void);
const char *get_page_scale(void);
/* the same value as a multiplier for CSS */
const char *get_page_scale_css(void);
int page_scale_is_valid(const char *value);

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
/* L3: the same colours as ONE key ("custom_vars"), for a page that cannot
   afford nine of the 32 context slots. The settings page uses this. */
void context_set_custom_theme_compact(TemplateContext *ctx,
                                      const CustomTheme *theme);
/* the palette as one CSS string: use this unless the page has colour pickers */
void context_set_custom_palette(TemplateContext *ctx, const CustomTheme *theme);
int parse_custom_theme_cookie(const char *value, CustomTheme *out);
void init_themes(const char *static_path);
void get_available_themes(char ***out_themes, int *out_count);
void set_default_locale(const char *locale);
char *get_locale(const char *default_locale);
void locale_parts(const char *locale, char *lang, size_t lang_size, char *region,
                  size_t region_size);

int is_engine_id_enabled(const char *engine_id);

/* L2/L6.10: the one table of ids this project has shipped and what each became.
   Returns the current id, or the input unchanged when it is not an old name. */
const char *legacy_engine_id(const char *id);

/* 1 when the id is an old name in that table, whatever it maps to now */
int found_old_name(const char *id);
int get_user_engines(char ***out_ids, int *out_count);
int user_engines_contains(const char *engine_id, char **ids, int count);
int opt_in_enabled_for(const char *provider_id);
char *get_default_search_engine(void);

void free_string_matrix(char ***matrix, int *inner_counts, int row_count);

void string_matrix_init(StringMatrix *matrix);
int string_matrix_append(StringMatrix *matrix, const char *const *values,
                         int field_count);
void string_matrix_free(StringMatrix *matrix);

int string_matrix_build_pagination(int page,
                                   char *(*href_builder)(int page, void *data),
                                   void *data, StringMatrix *out_matrix);

int is_known_theme(const char *theme);
int is_known_locale(const char *locale);

#endif
