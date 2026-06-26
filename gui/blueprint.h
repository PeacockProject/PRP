/* blueprint.h — declarative flavor install/config blueprints (see
 * peacock-ports/blueprints/SCHEMA.md). Pure data + logic + action execution, NO LVGL — so this
 * same unit backs the PRP installer wizard, the base first-boot OOBE, and the builder's headless
 * `--apply` mode. The LVGL rendering/capture of fields lives in the UI caller (prp_wizard.c). */
#ifndef PRP_BLUEPRINT_H
#define PRP_BLUEPRINT_H

#include <stddef.h>

typedef enum {
	BP_FIELD_DROPDOWN,
	BP_FIELD_TEXT,
	BP_FIELD_PASSWORD,
	BP_FIELD_TOGGLE,
	BP_FIELD_INFO,
} bp_field_type;

typedef struct {
	char *key;          /* answer key; NULL for info fields */
	bp_field_type type;
	char *label;
	char *options;      /* dropdown: newline-joined (lv_dropdown format); else NULL */
	char *def;          /* default value; may contain ${other_key} templating */
	char *placeholder;  /* text/password hint; else NULL */
	char *validate;     /* POSIX ERE checked on capture; else NULL */
	char *when;         /* per-field show-if expression; else NULL */
	int required;
} bp_field;

typedef enum { BP_PHASE_INSTALL, BP_PHASE_OOBE } bp_phase;

typedef struct {
	char *id;
	bp_phase phase;
	char *title;
	char *when;          /* stage show-if; else NULL */
	char **requires;     /* stage ids that must be done first */
	size_t n_requires;
	bp_field *fields;
	size_t n_fields;
	char *action;        /* inline POSIX sh; else NULL */
	char *action_script; /* relative path (fetched+verified separately); else NULL */
} bp_stage;

typedef struct {
	int schema;
	char *flavor;
	char *title;
	bp_stage *stages;
	size_t n_stages;
} bp_blueprint;

/* ---- blueprint load/free ---- */
/* Parse a blueprint TOML. Returns NULL on error with errbuf filled. */
bp_blueprint *bp_load(const char *path, char *errbuf, size_t errbufsz);
void bp_free(bp_blueprint *bp);

/* Stages of `phase`, topologically ordered by `requires`. Fills `out` (caller-provided array of
 * at least bp->n_stages pointers); returns the count placed. */
size_t bp_phase_order(const bp_blueprint *bp, bp_phase phase, const bp_stage **out);

/* ---- answers store (/peacock/etc/oobe/answers.toml) ---- */
typedef struct {
	char *flavor;
	int install_done;
	int oobe_done;
	char **keys;
	char **vals;
	char **st_ids;       /* stage_status ids */
	char **st_vals;      /* "done"|"pending"|"skipped" */
	size_t n, st_n, cap, st_cap;
} bp_answers;

bp_answers *bp_answers_load(const char *path); /* never NULL; empty store if file absent */
int bp_answers_save(const bp_answers *a, const char *path);
void bp_answers_free(bp_answers *a);
const char *bp_answers_get(const bp_answers *a, const char *key); /* NULL if unset */
void bp_answers_set(bp_answers *a, const char *key, const char *val); /* "" stored; password keys excluded by caller */
const char *bp_stage_status(const bp_answers *a, const char *id);    /* NULL if unset */
void bp_set_stage_status(bp_answers *a, const char *id, const char *status);

/* ---- expression + templating over the answers ---- */
/* `key OP value` (OP == / !=) joined by && / ||; "" or NULL => shown (1). */
int bp_when_eval(const char *expr, const bp_answers *a);
/* Expand ${key} from the answers; returns a malloc'd string (caller frees). */
char *bp_expand(const char *s, const bp_answers *a);
/* Validate `val` against a POSIX ERE; 1 if ok or regex NULL/empty. */
int bp_validate(const char *regex, const char *val);

/* ---- action execution (headless-capable; no LVGL) ----
 * Runs the stage's action/action_script under /bin/sh with: a sourced preamble defining
 * run_in_target() = `chroot "$ROOT" "$@"` and bp_log/bp_progress/bp_fail (which print the
 * STEP/PROGRESS/LOG/ERROR line-protocol), ROOT=<root>, and every answer as ANS_<key>. Password
 * values are injected from `secrets` (parallel key/val arrays) for THIS run only, never persisted.
 * Output lines are written to `sink_fd` (the UI parses them; pass STDOUT_FILENO for headless).
 * `scripts_dir` is where action_script paths resolve. Returns 0 on success. */
int bp_run_stage_action(const bp_stage *st, const bp_answers *a, const char *root,
                        const char *scripts_dir,
                        char *const *secret_keys, char *const *secret_vals, size_t n_secrets,
                        int sink_fd);

#endif /* PRP_BLUEPRINT_H */
