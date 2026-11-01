/* L6 follow-up: the provider table claims to be ordered by group, and the page
   relies on it - it walks the table once and prints a heading when the group
   changes. A row filed out of order therefore appeared under a second heading
   with the same name ("Answers" twice), which is how this was found.
   The claim is now checked instead of trusted. */
#include "../src/Providers/Providers.h"
#include <stdio.h>
#include <string.h>

int main(void) {
  ProviderRow rows[64];
  int n = providers_build(rows, 64);
  int failures = 0;

  printf("provider order: %d rows\n", n);

  /* each group appears in one contiguous run */
  ProviderGroup seen[16];
  int seen_count = 0;
  for (int i = 0; i < n; i++) {
    if (i > 0 && rows[i].group == rows[i - 1].group)
      continue;
    for (int s = 0; s < seen_count; s++) {
      if (seen[s] == rows[i].group) {
        printf("  FAIL group %s appears twice (%s after %s)\n",
               provider_group_label(rows[i].group), rows[i].id,
               rows[i - 1].id);
        failures++;
        break;
      }
    }
    if (seen_count < 16)
      seen[seen_count++] = rows[i].group;
  }

  /* no id twice */
  for (int i = 0; i < n; i++)
    for (int j = i + 1; j < n; j++)
      if (rows[i].id && rows[j].id && strcmp(rows[i].id, rows[j].id) == 0) {
        printf("  FAIL %s listed twice\n", rows[i].id);
        failures++;
      }

  /* every row can say where it goes, or says it does not go anywhere */
  for (int i = 0; i < n; i++) {
    if (!rows[i].id || !rows[i].id[0]) {
      printf("  FAIL a row has no id\n");
      failures++;
      continue;
    }
    if (!rows[i].label || !rows[i].label[0]) {
      printf("  FAIL %s has no label\n", rows[i].id);
      failures++;
    }
  }

  if (failures)
    printf("provider order: %d failed\n", failures);
  else
    printf("provider order: all groups contiguous, no duplicates\n");
  return failures ? 1 : 0;
}
