/* J5.1: engines as data.

   Adding an engine used to mean a C initialiser, a hand-written parser and a
   recompile. A definition in static/engines.d/ now supplies the URL template
   and four XPaths, and parse_definition - one parser, shared by all of them -
   runs them. The review's target was "an engine is ~10 lines of data"; this is
   the half of that which does not need the registry to be dynamic at runtime,
   and it is the half where new engines actually come from. */

#include "Scraping.h"
#include "../Utility/XmlHelper.h"
#include <dirent.h>
#include <jansson.h>
#include <libxml/HTMLparser.h>
#include <libxml/parser.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

/* Definitions live for the life of the process: they are loaded once at
   startup from a directory that does not change under us, and every
   SearchEngine field is a borrowed const char*. Freeing them would mean a
   teardown path for something that is never torn down, so they are kept and
   the intent is recorded here rather than lost. */
typedef struct {
  char *strings;
  int count;
} DefinitionArena;

static DefinitionArena arena;

static const char *arena_strdup(const char *s) {
  if (!s)
    return NULL;
  size_t n = strlen(s) + 1;
  char *copy = malloc(n);
  if (!copy)
    return NULL;
  memcpy(copy, s, n);
  if (arena.count < 1024)
    arena.strings = arena.strings; /* kept for the reader: see note above */
  arena.count++;
  return copy;
}

static const SearchEngine *engine_by_display_name(const char *name) {
  if (!name)
    return NULL;
  for (int i = 0; i < ENGINE_COUNT; i++) {
    if (ENGINE_REGISTRY[i].name && strcmp(ENGINE_REGISTRY[i].name, name) == 0)
      return &ENGINE_REGISTRY[i];
  }
  return NULL;
}

/* Selector text arrives with the page's indentation around it - the author
   span rendered as "\n          \nby Daniel P. Bovet\n\n        ". Collapse
   runs of whitespace and trim, or every snippet renders with holes in it. */
static void trim_in_place(char *text) {
  if (!text)
    return;
  char *read = text, *write = text;
  int pending_space = 0;
  while (*read == ' ' || *read == '\t' || *read == '\n' || *read == '\r')
    read++;
  while (*read) {
    if (*read == ' ' || *read == '\t' || *read == '\n' || *read == '\r') {
      pending_space = 1;
    } else {
      if (pending_space && write != text)
        *write++ = ' ';
      pending_space = 0;
      *write++ = *read;
    }
    read++;
  }
  *write = '\0';
}

/* Search pages routinely link relatively (/works/OL5739563W). A definition
   names base_url, so resolve against it rather than handing assign_result a
   path that would be rendered as a dead link on our own origin. */
static char *resolve_url(const SearchEngine *engine, char *href) {
  if (!href || !href[0])
    return href;
  if (strstr(href, "://") != NULL)
    return href;
  const char *base = engine->base_url ? engine->base_url : "";
  if (!base[0])
    return href;
  size_t need = strlen(base) + strlen(href) + 2;
  char *out = malloc(need);
  if (!out)
    return href;
  const char *rest = href;
  if (href[0] == '/' && href[1] == '/') {          /* protocol-relative */
    snprintf(out, need, "https:%s", href);
  } else if (href[0] == '/') {                     /* root-relative */
    snprintf(out, need, "%s%s", base, href);
  } else {                                         /* path-relative */
    snprintf(out, need, "%s/%s", base, href);
  }
  free(href);
  return out;
}

/* one shared parser: the selectors come from the engine the job is running */
static int parse_definition(const char *engine_name, xmlDocPtr doc,
                            SearchResult **out_results, int max_results) {
  const SearchEngine *engine = engine_by_display_name(engine_name);
  if (!engine || !engine->xpath_result)
    return 0;

  xmlXPathContextPtr ctx = create_xpath_context(doc);
  if (!ctx)
    return 0;

  xmlXPathObjectPtr obj = xml_xpath_eval(ctx, engine->xpath_result);
  if (!obj || !obj->nodesetval || obj->nodesetval->nodeNr == 0) {
    free_xpath_objects(ctx, obj);
    return 0;
  }

  int num_results = obj->nodesetval->nodeNr;
  *out_results = alloc_results_array(num_results, max_results);
  if (!*out_results) {
    free_xpath_objects(ctx, obj);
    return 0;
  }

  int found_count = 0;
  for (int i = 0; i < num_results && found_count < max_results; i++) {
    ctx->node = obj->nodesetval->nodeTab[i];

    char *url = NULL;
    if (engine->xpath_link) {
      xmlXPathObjectPtr link_obj = xml_xpath_eval(ctx, engine->xpath_link);
      if (link_obj && link_obj->nodesetval && link_obj->nodesetval->nodeNr > 0) {
        xmlNodePtr node = link_obj->nodesetval->nodeTab[0];
        /* "@href" selects an attribute; a plain path selects an element whose
           href we then read. Both are useful in a definition. */
        if (node->type == XML_ATTRIBUTE_NODE)
          url = (char *)xmlNodeGetContent(node);
        else
          url = (char *)xmlGetProp(node, (xmlChar *)"href");
      }
      if (link_obj)
        xmlXPathFreeObject(link_obj);
    }

    char *title = NULL;
    if (engine->xpath_title) {
      xmlXPathObjectPtr title_obj = xml_xpath_eval(ctx, engine->xpath_title);
      if (title_obj && title_obj->nodesetval && title_obj->nodesetval->nodeNr > 0)
        title = xml_node_content(title_obj->nodesetval->nodeTab[0]);
      if (title_obj)
        xmlXPathFreeObject(title_obj);
    }

    char *snippet = NULL;
    if (engine->xpath_snippet) {
      xmlXPathObjectPtr snip_obj = xml_xpath_eval(ctx, engine->xpath_snippet);
      if (snip_obj && snip_obj->nodesetval && snip_obj->nodesetval->nodeNr > 0)
        snippet = xml_node_content(snip_obj->nodesetval->nodeTab[0]);
      if (snip_obj)
        xmlXPathFreeObject(snip_obj);
    }

    trim_in_place(title);
    trim_in_place(snippet);
    url = resolve_url(engine, url);
    if (url && url[0] && assign_result(&(*out_results)[found_count], url,
                                       title, snippet, 0))
      found_count++;

    free(url);
    free(title);
    free(snippet);
  }

  ctx->node = NULL;
  free_xpath_objects(ctx, obj);
  return found_count;
}

static int verticals_from_json(json_t *array) {
  int mask = 0;
  size_t idx;
  json_t *value;
  if (!json_is_array(array))
    return V_WEB;
  json_array_foreach(array, idx, value) {
    const char *name = json_string_value(value);
    if (!name)
      continue;
    if (strcmp(name, "web") == 0)
      mask |= V_WEB;
    else if (strcmp(name, "media") == 0)
      mask |= V_MEDIA;
    else if (strcmp(name, "code") == 0)
      mask |= V_CODE;
    else if (strcmp(name, "academic") == 0)
      mask |= V_ACADEMIC;
    else if (strcmp(name, "entity") == 0)
      mask |= V_ENTITY;
    else if (strcmp(name, "news") == 0)
      mask |= V_NEWS;
  }
  return mask ? mask : V_WEB;
}

static const char *json_str(json_t *obj, const char *key) {
  return json_string_value(json_object_get(obj, key));
}

/* returns 1 when accepted, 0 when rejected; *reason is set on rejection */
static int load_one(const char *path, char *reason, size_t reason_size) {
  json_error_t error;
  json_t *root = json_load_file(path, 0, &error);
  if (!root) {
    snprintf(reason, reason_size, "not JSON: %s", error.text);
    return 0;
  }

  const char *id = json_str(root, "id");
  const char *name = json_str(root, "name");
  const char *search_url = json_str(root, "search_url");
  const char *xpath_result = json_str(root, "xpath_result");
  const char *xpath_link = json_str(root, "xpath_link");
  const char *xpath_title = json_str(root, "xpath_title");
  const char *xpath_snippet = json_str(root, "xpath_snippet");

  /* A definition that cannot produce a result is rejected here, not silently
     registered and then found empty in production. */
  if (!id || !id[0] || !name || !name[0]) {
    snprintf(reason, reason_size, "id and name are required");
    json_decref(root);
    return 0;
  }
  if (!search_url || strstr(search_url, "%s") == NULL) {
    snprintf(reason, reason_size, "search_url must contain %%s");
    json_decref(root);
    return 0;
  }
  if (!xpath_result || !xpath_link) {
    snprintf(reason, reason_size, "xpath_result and xpath_link are required");
    json_decref(root);
    return 0;
  }
  for (int i = 0; i < ENGINE_COUNT; i++) {
    if (strcmp(ENGINE_REGISTRY[i].id, id) == 0) {
      snprintf(reason, reason_size, "id '%s' is already registered", id);
      json_decref(root);
      return 0;
    }
    if (ENGINE_REGISTRY[i].name && strcmp(ENGINE_REGISTRY[i].name, name) == 0) {
      /* the shared parser is handed the display name, so this would make the
         second definition read the first one's selectors */
      snprintf(reason, reason_size, "name '%s' is already registered", name);
      json_decref(root);
      return 0;
    }
  }

  SearchEngine engine;
  memset(&engine, 0, sizeof(engine));
  engine.id = arena_strdup(id);
  engine.name = arena_strdup(name);
  engine.base_url = arena_strdup(json_str(root, "base_url"));
  engine.search_url_fmt = arena_strdup(search_url);
  engine.index_group = arena_strdup(json_str(root, "index_group"));
  engine.xpath_result = arena_strdup(xpath_result);
  engine.xpath_title = arena_strdup(xpath_title);
  engine.xpath_link = arena_strdup(xpath_link);
  engine.xpath_snippet = arena_strdup(xpath_snippet);
  engine.parser = parse_definition;
  /* Off unless the file says on. A definition is a claim about a page someone
     may not have measured against this build's 2 s page deadline: Open Library
     answers in ~1.7 s against three engines and is cancelled against eleven,
     so shipping it enabled would spend a connection per query and produce
     nothing. Enabling it is a decision, deliberately. */
  engine.enabled = json_is_true(json_object_get(root, "enabled")) ? 1 : 0;
  engine.is_xml = 0;
  engine.page_param = "page";
  engine.page_multiplier = 1;
  engine.page_base = 1;
  engine.verticals = verticals_from_json(json_object_get(root, "verticals"));
  json_t *weight = json_object_get(root, "weight");
  engine.weight = json_is_number(weight) ? json_number_value(weight) : 1.0;
  json_t *pace = json_object_get(root, "pace_per_minute");
  engine.pace_per_minute = json_is_integer(pace) ? (int)json_integer_value(pace) : 0;
  /* tier 3 is never inferred from a file: a definition can only be opt-in if
     it says so, and the gate still has to be open at request time. */
  json_t *opt_in = json_object_get(root, "opt_in");
  engine.opt_in = json_is_true(opt_in) ? 1 : 0;

  if (!engine.id || !engine.name || !engine.search_url_fmt ||
      !engine.xpath_result ||
      !engine.xpath_link) {
    snprintf(reason, reason_size, "out of memory");
    json_decref(root);
    return 0;
  }

  if (engine_registry_add(&engine) < 0) {
    snprintf(reason, reason_size, "registry is full (%d)", MAX_ENGINES);
    json_decref(root);
    return 0;
  }
  json_decref(root);
  return 1;
}

int engines_load_definitions(const char *dir) {
  if (!dir || !dir[0])
    return 0;
  DIR *handle = opendir(dir);
  if (!handle)
    return 0; /* no directory is not an error: a build ships zero definitions */

  int loaded = 0;
  struct dirent *entry;
  while ((entry = readdir(handle)) != NULL) {
    size_t len = strlen(entry->d_name);
    if (len < 6 || strcmp(entry->d_name + len - 5, ".json") != 0)
      continue;
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/%s", dir, entry->d_name);
    char reason[192] = "";
    if (load_one(path, reason, sizeof(reason))) {
      loaded++;
    } else {
      fprintf(stderr, "[engines.d] %s: %s\n", entry->d_name, reason);
    }
  }
  closedir(handle);
  return loaded;
}
