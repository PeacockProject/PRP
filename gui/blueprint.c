/* blueprint.c — see blueprint.h. Pure logic, no LVGL. */
#define _POSIX_C_SOURCE 200809L
#include "blueprint.h"
#include "toml.h"
#include "bp_verify.h"

#include <regex.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>

/* ---- small helpers ---- */
static char *xdup(const char *s) { return s ? strdup(s) : NULL; }

/* Owned string for table key, or NULL. tomlc99 returns a malloc'd .u.s we take ownership of. */
static char *tstr(toml_table_t *t, const char *k) {
	toml_datum_t d = toml_string_in(t, k);
	return d.ok ? d.u.s : NULL;
}

static bp_field_type field_type(const char *ty) {
	if (!ty) return BP_FIELD_TEXT;
	if (!strcmp(ty, "dropdown")) return BP_FIELD_DROPDOWN;
	if (!strcmp(ty, "password")) return BP_FIELD_PASSWORD;
	if (!strcmp(ty, "toggle"))   return BP_FIELD_TOGGLE;
	if (!strcmp(ty, "info"))     return BP_FIELD_INFO;
	return BP_FIELD_TEXT;
}

/* Join a TOML array of strings into a newline-separated list (lv_dropdown options format). */
static char *join_opts(toml_array_t *arr) {
	int n = toml_array_nelem(arr);
	size_t cap = 1;
	char *out = malloc(cap);
	out[0] = '\0';
	size_t len = 0;
	for (int i = 0; i < n; i++) {
		toml_datum_t d = toml_string_at(arr, i);
		if (!d.ok) continue;
		size_t add = strlen(d.u.s) + 1; /* +1 for '\n' or NUL */
		out = realloc(out, len + add + 1);
		if (len) out[len++] = '\n';
		memcpy(out + len, d.u.s, strlen(d.u.s));
		len += strlen(d.u.s);
		out[len] = '\0';
		free(d.u.s);
	}
	return out;
}

/* ---- load / free ---- */
bp_blueprint *bp_load(const char *path, char *errbuf, size_t errbufsz) {
	FILE *fp = fopen(path, "r");
	if (!fp) { snprintf(errbuf, errbufsz, "cannot open %s", path); return NULL; }
	char terr[200];
	toml_table_t *root = toml_parse_file(fp, terr, sizeof terr);
	fclose(fp);
	if (!root) { snprintf(errbuf, errbufsz, "parse %s: %s", path, terr); return NULL; }

	bp_blueprint *bp = calloc(1, sizeof *bp);
	toml_datum_t sd = toml_int_in(root, "schema");
	bp->schema = sd.ok ? (int)sd.u.i : 0;
	bp->kind   = tstr(root, "kind");
	bp->flavor = tstr(root, "flavor");
	bp->title  = tstr(root, "title");
	toml_table_t *arch = toml_table_in(root, "archive");
	if (arch) {
		bp->archive_url    = tstr(arch, "url");
		bp->archive_sha256 = tstr(arch, "sha256");
	}

	toml_array_t *stages = toml_array_in(root, "stage");
	if (stages) {
		int n = toml_array_nelem(stages);
		bp->stages = calloc(n > 0 ? n : 1, sizeof(bp_stage));
		for (int i = 0; i < n; i++) {
			toml_table_t *stab = toml_table_at(stages, i);
			if (!stab) continue;
			bp_stage *s = &bp->stages[bp->n_stages++];
			s->id          = tstr(stab, "id");
			s->title       = tstr(stab, "title");
			s->description = tstr(stab, "description");
			s->when        = tstr(stab, "when");
			s->action        = tstr(stab, "action");
			s->action_script = tstr(stab, "action_script");
			if (!s->action_script) s->action_script = tstr(stab, "script"); /* declarative form */
			char *ph = tstr(stab, "phase");
			if (ph)
				s->phase = !strcmp(ph, "install") ? BP_PHASE_INSTALL : BP_PHASE_OOBE;
			else /* default from the blueprint's kind */
				s->phase = (bp->kind && !strcmp(bp->kind, "install")) ? BP_PHASE_INSTALL : BP_PHASE_OOBE;
			free(ph);

			toml_array_t *req = toml_array_in(stab, "requires");
			if (req) {
				int rn = toml_array_nelem(req);
				s->requires = calloc(rn > 0 ? rn : 1, sizeof(char *));
				for (int j = 0; j < rn; j++) {
					toml_datum_t rd = toml_string_at(req, j);
					if (rd.ok) s->requires[s->n_requires++] = rd.u.s;
				}
			}

			toml_array_t *fields = toml_array_in(stab, "field");
			if (fields) {
				int fn = toml_array_nelem(fields);
				s->fields = calloc(fn > 0 ? fn : 1, sizeof(bp_field));
				for (int j = 0; j < fn; j++) {
					toml_table_t *ft = toml_table_at(fields, j);
					if (!ft) continue;
					bp_field *f = &s->fields[s->n_fields++];
					f->key         = tstr(ft, "key");
					f->label       = tstr(ft, "label");
					f->def         = tstr(ft, "default");
					f->placeholder = tstr(ft, "placeholder");
					f->validate    = tstr(ft, "validate");
					f->when        = tstr(ft, "when");
					char *ty = tstr(ft, "type");
					f->type = field_type(ty);
					free(ty);
					toml_datum_t rq = toml_bool_in(ft, "required");
					f->required = rq.ok ? rq.u.b : 0;
					toml_array_t *opts = toml_array_in(ft, "options");
					if (opts) f->options = join_opts(opts);
				}
			}
		}
	}
	toml_free(root);
	return bp;
}

void bp_free(bp_blueprint *bp) {
	if (!bp) return;
	for (size_t i = 0; i < bp->n_stages; i++) {
		bp_stage *s = &bp->stages[i];
		free(s->id); free(s->title); free(s->description); free(s->when);
		free(s->action); free(s->action_script);
		for (size_t j = 0; j < s->n_requires; j++) free(s->requires[j]);
		free(s->requires);
		for (size_t j = 0; j < s->n_fields; j++) {
			bp_field *f = &s->fields[j];
			free(f->key); free(f->label); free(f->options); free(f->def);
			free(f->placeholder); free(f->validate); free(f->when);
		}
		free(s->fields);
	}
	free(bp->stages); free(bp->kind); free(bp->flavor); free(bp->title);
	free(bp->archive_url); free(bp->archive_sha256); free(bp);
}

/* ---- phase ordering (stable topo sort by `requires`) ---- */
size_t bp_phase_order(const bp_blueprint *bp, bp_phase phase, const bp_stage **out) {
	char emitted[256] = {0}; /* index-by-stage flag; blueprints are small */
	size_t count = 0;
	size_t want = 0;
	for (size_t i = 0; i < bp->n_stages; i++)
		if (bp->stages[i].phase == phase) want++;

	for (size_t pass = 0; pass < bp->n_stages + 1 && count < want; pass++) {
		int progress = 0;
		for (size_t i = 0; i < bp->n_stages && i < 256; i++) {
			const bp_stage *s = &bp->stages[i];
			if (s->phase != phase || emitted[i]) continue;
			int ready = 1;
			for (size_t r = 0; r < s->n_requires; r++) {
				/* required stage must be emitted already if it's in this phase */
				for (size_t k = 0; k < bp->n_stages && k < 256; k++) {
					if (bp->stages[k].id && s->requires[r] &&
					    !strcmp(bp->stages[k].id, s->requires[r]) &&
					    bp->stages[k].phase == phase && !emitted[k]) {
						ready = 0;
					}
				}
			}
			if (ready) { out[count++] = s; emitted[i] = 1; progress = 1; }
		}
		if (!progress) break; /* cycle / unsatisfiable — stop */
	}
	return count;
}

/* ---- answers store ---- */
static void ans_grow(bp_answers *a) {
	if (a->n + 1 > a->cap) { a->cap = a->cap ? a->cap * 2 : 8;
		a->keys = realloc(a->keys, a->cap * sizeof(char *));
		a->vals = realloc(a->vals, a->cap * sizeof(char *)); }
}
static void st_grow(bp_answers *a) {
	if (a->st_n + 1 > a->st_cap) { a->st_cap = a->st_cap ? a->st_cap * 2 : 8;
		a->st_ids  = realloc(a->st_ids,  a->st_cap * sizeof(char *));
		a->st_vals = realloc(a->st_vals, a->st_cap * sizeof(char *)); }
}

const char *bp_answers_get(const bp_answers *a, const char *key) {
	for (size_t i = 0; i < a->n; i++)
		if (!strcmp(a->keys[i], key)) return a->vals[i];
	return NULL;
}
void bp_answers_set(bp_answers *a, const char *key, const char *val) {
	for (size_t i = 0; i < a->n; i++)
		if (!strcmp(a->keys[i], key)) { free(a->vals[i]); a->vals[i] = xdup(val ? val : ""); return; }
	ans_grow(a);
	a->keys[a->n] = xdup(key);
	a->vals[a->n] = xdup(val ? val : "");
	a->n++;
}
const char *bp_stage_status(const bp_answers *a, const char *id) {
	for (size_t i = 0; i < a->st_n; i++)
		if (!strcmp(a->st_ids[i], id)) return a->st_vals[i];
	return NULL;
}
void bp_set_stage_status(bp_answers *a, const char *id, const char *status) {
	for (size_t i = 0; i < a->st_n; i++)
		if (!strcmp(a->st_ids[i], id)) { free(a->st_vals[i]); a->st_vals[i] = xdup(status); return; }
	st_grow(a);
	a->st_ids[a->st_n]  = xdup(id);
	a->st_vals[a->st_n] = xdup(status);
	a->st_n++;
}

bp_answers *bp_answers_load(const char *path) {
	bp_answers *a = calloc(1, sizeof *a);
	FILE *fp = fopen(path, "r");
	if (!fp) return a; /* empty store */
	char terr[200];
	toml_table_t *root = toml_parse_file(fp, terr, sizeof terr);
	fclose(fp);
	if (!root) return a;
	toml_table_t *meta = toml_table_in(root, "meta");
	if (meta) {
		a->flavor = tstr(meta, "flavor");
		toml_datum_t i1 = toml_bool_in(meta, "install_done"); a->install_done = i1.ok ? i1.u.b : 0;
		toml_datum_t i2 = toml_bool_in(meta, "oobe_done");    a->oobe_done    = i2.ok ? i2.u.b : 0;
	}
	toml_table_t *ans = toml_table_in(root, "answers");
	if (ans) {
		for (int i = 0; ; i++) {
			const char *k = toml_key_in(ans, i);
			if (!k) break;
			char *v = tstr(ans, k);
			if (v) { bp_answers_set(a, k, v); free(v); }
		}
	}
	toml_table_t *ss = toml_table_in(root, "stage_status");
	if (ss) {
		for (int i = 0; ; i++) {
			const char *k = toml_key_in(ss, i);
			if (!k) break;
			char *v = tstr(ss, k);
			if (v) { bp_set_stage_status(a, k, v); free(v); }
		}
	}
	toml_free(root);
	return a;
}

int bp_answers_save(const bp_answers *a, const char *path) {
	/* write atomically via <path>.tmp */
	char tmp[1024];
	snprintf(tmp, sizeof tmp, "%s.tmp", path);
	FILE *fp = fopen(tmp, "w");
	if (!fp) return -1;
	fprintf(fp, "[meta]\nschema = 1\n");
	if (a->flavor) fprintf(fp, "flavor = \"%s\"\n", a->flavor);
	fprintf(fp, "install_done = %s\noobe_done = %s\n\n",
	        a->install_done ? "true" : "false", a->oobe_done ? "true" : "false");
	fprintf(fp, "[answers]\n");
	for (size_t i = 0; i < a->n; i++)
		fprintf(fp, "%s = \"%s\"\n", a->keys[i], a->vals[i]);
	fprintf(fp, "\n[stage_status]\n");
	for (size_t i = 0; i < a->st_n; i++)
		fprintf(fp, "%s = \"%s\"\n", a->st_ids[i], a->st_vals[i]);
	fclose(fp);
	return rename(tmp, path);
}

void bp_answers_free(bp_answers *a) {
	if (!a) return;
	for (size_t i = 0; i < a->n; i++) { free(a->keys[i]); free(a->vals[i]); }
	for (size_t i = 0; i < a->st_n; i++) { free(a->st_ids[i]); free(a->st_vals[i]); }
	free(a->keys); free(a->vals); free(a->st_ids); free(a->st_vals); free(a->flavor); free(a);
}

/* ---- ${key} templating ---- */
char *bp_expand(const char *s, const bp_answers *a) {
	if (!s) return xdup("");
	size_t cap = strlen(s) + 1, len = 0;
	char *out = malloc(cap);
	for (const char *p = s; *p; ) {
		if (p[0] == '$' && p[1] == '{') {
			const char *end = strchr(p + 2, '}');
			if (end) {
				char key[128];
				size_t kl = (size_t)(end - (p + 2));
				if (kl < sizeof key) {
					memcpy(key, p + 2, kl); key[kl] = '\0';
					const char *v = bp_answers_get(a, key);
					if (!v) v = "";
					size_t vl = strlen(v);
					out = realloc(out, len + vl + 1);
					memcpy(out + len, v, vl); len += vl;
					p = end + 1;
					continue;
				}
			}
		}
		out = realloc(out, len + 2);
		out[len++] = *p++;
	}
	out[len] = '\0';
	return out;
}

/* ---- `when` expression: key OP value joined by && / || ---- */
static void trim(char *s) {
	char *p = s; while (*p == ' ' || *p == '\t') p++;
	if (p != s) memmove(s, p, strlen(p) + 1);
	size_t n = strlen(s);
	while (n && (s[n-1] == ' ' || s[n-1] == '\t')) s[--n] = '\0';
}
/* evaluate a single `key OP value` atom */
static int eval_atom(const char *atom, const bp_answers *a) {
	char buf[256];
	snprintf(buf, sizeof buf, "%s", atom);
	char *op = strstr(buf, "==");
	int neg = 0;
	if (!op) { op = strstr(buf, "!="); neg = 1; }
	if (!op) return 1; /* malformed → shown */
	char key[256], val[256];
	*op = '\0';
	snprintf(key, sizeof key, "%s", buf);
	snprintf(val, sizeof val, "%s", op + 2);
	trim(key); trim(val);
	/* strip surrounding quotes on val */
	size_t vl = strlen(val);
	if (vl >= 2 && (val[0] == '"' || val[0] == '\'') && val[vl-1] == val[0]) {
		val[vl-1] = '\0'; memmove(val, val + 1, strlen(val) + 1);
	}
	const char *cur = bp_answers_get(a, key);
	if (!cur) cur = "";
	int eq = !strcmp(cur, val);
	return neg ? !eq : eq;
}
int bp_when_eval(const char *expr, const bp_answers *a) {
	if (!expr || !*expr) return 1;
	char buf[512];
	snprintf(buf, sizeof buf, "%s", expr);
	/* OR of AND-terms */
	char *or_save = NULL;
	for (char *orterm = strtok_r(buf, "|", &or_save); orterm; orterm = strtok_r(NULL, "|", &or_save)) {
		if (!*orterm) continue; /* skip the empty token from "||" */
		char term[512];
		snprintf(term, sizeof term, "%s", orterm);
		int all = 1;
		char *and_save = NULL;
		int any = 0;
		for (char *at = strtok_r(term, "&", &and_save); at; at = strtok_r(NULL, "&", &and_save)) {
			if (!*at) continue;
			any = 1;
			if (!eval_atom(at, a)) { all = 0; break; }
		}
		if (any && all) return 1;
	}
	return 0;
}

/* ---- validation ---- */
int bp_validate(const char *regex, const char *val) {
	if (!regex || !*regex) return 1;
	regex_t re;
	if (regcomp(&re, regex, REG_EXTENDED | REG_NOSUB) != 0) return 1; /* bad regex → don't block */
	int ok = regexec(&re, val ? val : "", 0, NULL, 0) == 0;
	regfree(&re);
	return ok;
}

/* ---- action execution ---- */
static const char *PREAMBLE =
	"run_in_target(){ chroot \"$ROOT\" \"$@\"; }\n"
	"bp_log(){ echo \"LOG $*\"; }\n"
	"bp_progress(){ echo \"PROGRESS $1\"; }\n"
	"bp_fail(){ echo \"ERROR $*\"; exit 1; }\n";

int bp_run_stage_action(const bp_stage *st, const bp_answers *a, const char *root,
                        const char *scripts_dir,
                        char *const *secret_keys, char *const *secret_vals, size_t n_secrets,
                        int sink_fd) {
	if (!st->action && !st->action_script) return 0;

	/* assemble the script: preamble + (inline action | source the action_script) */
	char *body;
	if (st->action_script) {
		size_t need = strlen(scripts_dir ? scripts_dir : ".") + strlen(st->action_script) + 8;
		body = malloc(need);
		snprintf(body, need, ". '%s/%s'\n", scripts_dir ? scripts_dir : ".", st->action_script);
	} else {
		body = xdup(st->action);
	}
	size_t scriptlen = strlen(PREAMBLE) + strlen(body) + 1;
	char *script = malloc(scriptlen);
	snprintf(script, scriptlen, "%s%s", PREAMBLE, body);
	free(body);

	pid_t pid = fork();
	if (pid == 0) {
		if (sink_fd >= 0) { dup2(sink_fd, 1); dup2(sink_fd, 2); }
		setenv("ROOT", root ? root : "/", 1);
		for (size_t i = 0; i < a->n; i++) {
			char env[160];
			snprintf(env, sizeof env, "ANS_%s", a->keys[i]);
			setenv(env, a->vals[i], 1);
		}
		for (size_t i = 0; i < n_secrets; i++) {
			char env[160];
			snprintf(env, sizeof env, "ANS_%s", secret_keys[i]);
			setenv(env, secret_vals[i], 1);
		}
		execlp("sh", "sh", "-c", script, (char *)NULL);
		_exit(127);
	}
	free(script);
	if (pid < 0) return -1;
	int status = 0;
	waitpid(pid, &status, 0);
	return (WIFEXITED(status) && WEXITSTATUS(status) == 0) ? 0 : -1;
}

/* ---- fetch + verify (curl/wget; feather not involved) ---- */
static int fetch_url(const char *url, const char *out) {
	pid_t pid = fork();
	if (pid == 0) {
		execlp("curl", "curl", "-fsSL", "--retry", "2", "-o", out, url, (char *)NULL);
		execlp("wget", "wget", "-q", "-O", out, url, (char *)NULL);
		_exit(127);
	}
	if (pid < 0) return -1;
	int st = 0;
	waitpid(pid, &st, 0);
	return (WIFEXITED(st) && WEXITSTATUS(st) == 0) ? 0 : -1;
}

int bp_fetch_verify(const char *url, const char *pubkey_path, const char *out_path,
                    char *err, size_t errsz) {
	char sigurl[1024], sigout[1024];
	snprintf(sigurl, sizeof sigurl, "%s.sig", url);
	snprintf(sigout, sizeof sigout, "%s.sig", out_path);
	if (fetch_url(url, out_path) != 0) { snprintf(err, errsz, "fetch failed: %s", url); return -1; }
	if (fetch_url(sigurl, sigout) != 0) { snprintf(err, errsz, "fetch failed: %s", sigurl); return -1; }
	if (bp_verify_file(out_path, sigout, pubkey_path, err, errsz) != 0) {
		remove(out_path); /* never leave an unverified blueprint on disk */
		return -1;
	}
	return 0;
}

int bp_fetch_flavors(const char *base_url, const char *pubkey_path, char *out, size_t outcap,
                     char *err, size_t errsz) {
	char url[600];
	snprintf(url, sizeof url, "%s/index.toml", base_url);
	if (bp_fetch_verify(url, pubkey_path, "/tmp/prp-bp-index.toml", err, errsz) != 0) return -1;
	FILE *fp = fopen("/tmp/prp-bp-index.toml", "r");
	if (!fp) { snprintf(err, errsz, "no index"); return -1; }
	char terr[200];
	toml_table_t *root = toml_parse_file(fp, terr, sizeof terr);
	fclose(fp);
	if (!root) { snprintf(err, errsz, "parse index: %s", terr); return -1; }
	toml_array_t *arr = toml_array_in(root, "flavor");
	out[0] = '\0';
	size_t len = 0;
	int count = 0;
	if (arr) {
		int n = toml_array_nelem(arr);
		for (int i = 0; i < n; i++) {
			toml_table_t *t = toml_table_at(arr, i);
			if (!t) continue;
			char *nm = tstr(t, "name");
			if (!nm) nm = tstr(t, "id");
			if (!nm) continue;
			size_t nl = strlen(nm);
			if (len + nl + 2 < outcap) {
				if (len) out[len++] = '\n';
				memcpy(out + len, nm, nl);
				len += nl;
				out[len] = '\0';
				count++;
			}
			free(nm);
		}
	}
	toml_free(root);
	if (count == 0) { snprintf(err, errsz, "index lists no flavors"); return -1; }
	return count;
}
