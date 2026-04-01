#include "Utility.h"
#include <beaker.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int hex_to_int(char c) {
  if (c >= '0' && c <= '9')
    return c - '0';
  if (c >= 'a' && c <= 'f')
    return c - 'a' + 10;
  if (c >= 'A' && c <= 'F')
    return c - 'A' + 10;
  return -1;
}

char *get_theme(const char *default_theme) {
  char *cookie = get_cookie("theme");
  if (cookie &&
      (strcmp(cookie, "light") == 0 ||
       strcmp(cookie, "dark") == 0)) {
    return cookie;
  }
  free(cookie);
  return strdup(default_theme);
}

char *get_locale(const char *default_locale) {
  char *cookie = get_cookie("locale");
  if (cookie && beaker_get_locale_meta(cookie) != NULL) {
    return cookie;
  }
  free(cookie);
  return strdup(default_locale);
}

int add_link_to_collection(const char *href, const char *label,
                           const char *class_name, char ****collection,
                           int **inner_counts, int current_count) {
  char ***old_collection = *collection;
  int *old_inner_counts = *inner_counts;
  char ***new_collection =
      (char ***)malloc(sizeof(char **) * (current_count + 1));
  int *new_inner_counts =
      (int *)malloc(sizeof(int) * (current_count + 1));

  if (!new_collection || !new_inner_counts) {
    free(new_collection);
    free(new_inner_counts);
    return current_count;
  }

  if (*collection && current_count > 0) {
    memcpy(new_collection, *collection, sizeof(char **) * current_count);
  }
  if (*inner_counts && current_count > 0) {
    memcpy(new_inner_counts, *inner_counts, sizeof(int) * current_count);
  }

  *collection = new_collection;
  *inner_counts = new_inner_counts;

  (*collection)[current_count] =
      (char **)malloc(sizeof(char *) * LINK_FIELD_COUNT);
  if (!(*collection)[current_count]) {
    *collection = old_collection;
    *inner_counts = old_inner_counts;
    free(new_collection);
    free(new_inner_counts);
    return current_count;
  }

  (*collection)[current_count][0] = strdup(href ? href : "");
  (*collection)[current_count][1] = strdup(label ? label : "");
  (*collection)[current_count][2] = strdup(class_name ? class_name : "");

  if (!(*collection)[current_count][0] || !(*collection)[current_count][1] ||
      !(*collection)[current_count][2]) {
    free((*collection)[current_count][0]);
    free((*collection)[current_count][1]);
    free((*collection)[current_count][2]);
    free((*collection)[current_count]);
    *collection = old_collection;
    *inner_counts = old_inner_counts;
    free(new_collection);
    free(new_inner_counts);
    return current_count;
  }

  (*inner_counts)[current_count] = LINK_FIELD_COUNT;

  free(old_collection);
  free(old_inner_counts);
  return current_count + 1;
}

int build_pagination(int page, const char *locale,
                     char *(*href_builder)(int page, void *data), void *data,
                     char ****out_matrix, int **out_inner_counts) {
  enum { PAGER_WINDOW_SIZE = 5 };

  *out_matrix = NULL;
  *out_inner_counts = NULL;
  int count = 0;

  int pager_start = page <= 3 ? 1 : page - 2;
  int pager_end = pager_start + PAGER_WINDOW_SIZE - 1;

  if (page > 1) {
    char *href = href_builder(page - 1, data);
    count = add_link_to_collection(href, "<", "pagination-btn prev",
                                   out_matrix, out_inner_counts, count);
    free(href);
  }

  for (int i = pager_start; i <= pager_end; i++) {
    char label[16];
    snprintf(label, sizeof(label), "%d", i);
    char *href = href_builder(i, data);
    count = add_link_to_collection(
        href, label,
        i == page ? "pagination-btn pagination-current" : "pagination-btn",
        out_matrix, out_inner_counts, count);
    free(href);
  }

  char *href = href_builder(page + 1, data);
  count = add_link_to_collection(href, ">", "pagination-btn next",
                                 out_matrix, out_inner_counts, count);
  free(href);

  return count;
}
