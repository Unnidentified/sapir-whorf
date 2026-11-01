/* L3: the secrets file. No network, no server.
 *
 * The file is the whole point of this item: a credential written by the page
 * must land in a 0600 file the archive never carries, must never be readable
 * back through the page, and must never reach a log line. These checks exercise
 * the storage rules directly, with OMNI_SECRETS_FILE pointing into a temp dir.
 */
#include "Utility/Secrets.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static int checks, failures;
static void check(int ok, const char *what) {
  checks++;
  if (!ok) {
    printf("FAIL %s\n", what);
    failures++;
  }
}

static unsigned mode_of(const char *path) {
  struct stat st;
  if (stat(path, &st) != 0)
    return 0;
  return (unsigned)(st.st_mode & 07777);
}

int main(void) {
  const char *dir = getenv("L3_TMPDIR");
  if (!dir || !dir[0]) {
    printf("L3 secrets: SKIP -- L3_TMPDIR not set\n");
    return 0;
  }
  char file[512];
  snprintf(file, sizeof(file), "%s/secrets.ini", dir);
  setenv("OMNI_SECRETS_FILE", file, 1);
  unsetenv("OMNI_REDDIT_CLIENT_ID");

  /* 1. absent to begin with */
  secrets_load();
  const char *value = NULL;
  check(secrets_get(SK_REDDIT_CLIENT_ID, &value) == SECRET_SOURCE_ABSENT,
        "a fresh store reports the client id absent");

  /* 2. write one, and the file is 0600 */
  check(secrets_set(SK_REDDIT_CLIENT_ID, "abcdef123456") == 0,
        "the client id is written");
  check(mode_of(file) == 0600, "the file is 0600 after the first save");
  check(mode_of(dir) == 0700, "the directory is 0700");

  /* 3. read back, and the source says the file */
  secrets_reload_after_save();
  value = NULL;
  check(secrets_get(SK_REDDIT_CLIENT_ID, &value) == SECRET_SOURCE_FILE &&
            value && strcmp(value, "abcdef123456") == 0,
        "the client id reads back from the file");

  /* 4. a second save keeps the mode: rename() plus an explicit chmod */
  check(secrets_set(SK_REDDIT_USERNAME, "someone") == 0, "the username is saved");
  check(mode_of(file) == 0600, "the file is still 0600 after the second save");

  /* 5. the masked form shows the last four characters and nothing else */
  char masked[128];
  secrets_mask(SK_REDDIT_CLIENT_ID, masked, sizeof(masked));
  check(strstr(masked, "3456") != NULL, "the mask keeps the last four");
  check(strstr(masked, "abcdef12") == NULL, "the mask hides the rest");

  /* 6. a credential is marked as one; a url is not */
  check(secrets_is_credential(SK_REDDIT_CLIENT_SECRET),
        "the client secret is a credential");
  check(!secrets_is_credential(SK_SEARXNG_URL), "a url is not a credential");

  /* 7. the environment wins over the file, and says so */
  setenv("OMNI_REDDIT_CLIENT_ID", "from-env", 1);
  value = NULL;
  check(secrets_get(SK_REDDIT_CLIENT_ID, &value) == SECRET_SOURCE_ENV,
        "the environment overrides the file");
  secrets_mask(SK_REDDIT_CLIENT_ID, masked, sizeof(masked));
  check(strstr(masked, "from-env") == NULL, "the mask hides an env value too");
  unsetenv("OMNI_REDDIT_CLIENT_ID");
  secrets_reload_after_save();

  /* 8. clearing removes the key */
  check(secrets_set(SK_REDDIT_CLIENT_ID, "") == 0, "the client id is cleared");
  secrets_reload_after_save();
  check(secrets_get(SK_REDDIT_CLIENT_ID, &value) == SECRET_SOURCE_ABSENT,
        "a cleared key reads back absent");
  FILE *fh = fopen(file, "r");
  int found = 0;
  if (fh) {
    char line[1024];
    while (fgets(line, sizeof(line), fh))
      if (strstr(line, "abcdef123456"))
        found = 1;
    fclose(fh);
  }
  check(!found, "the cleared value is gone from the file");

  /* 9. an unsafe mode is refused, and the refusal is on stderr, not silent */
  chmod(file, 0644);
  check(secrets_load() == -1, "a world-readable store is refused");
  chmod(file, 0600);
  check(secrets_load() == 0, "a 0600 store is read again once fixed");

  /* 10. an admin token exists, is long, and matches only itself */
  const char *token = secrets_admin_token();
  check(token && strlen(token) >= 48, "an admin token was generated");
  check(secrets_admin_token_matches(token), "the token matches itself");
  check(!secrets_admin_token_matches("not-the-token"), "a wrong token is refused");
  check(!secrets_admin_token_matches(""), "an empty token is refused");

  printf("L3 secrets: %d checks, %d failed\n", checks, failures);
  return failures ? 1 : 0;
}
