#ifndef FM_JOB_H
#define FM_JOB_H

/* ─────────────────────────────────────────────────────────────────── *
 *  Background job with a modal progress dialog.
 *
 *  job_run() starts `fn` in a worker thread and spins a nested main
 *  loop until it returns – the UI keeps repainting, Cancel works, and
 *  the worker never touches GTK.  Overwrite questions from the
 *  engine (FsCtx.conflict) are marshalled to the main thread.
 * ─────────────────────────────────────────────────────────────────── */

#include "fm.h"

typedef struct Job Job;
typedef void (*JobFunc)(Job *job, gpointer data);

struct Job {
    FsCtx      ctx;          /* pass &job->ctx to fs_* functions       */
    FM        *fm;

    /* shared state – guarded by lock */
    GMutex     lock;
    gchar     *phase;
    gchar     *file;
    gchar     *from;         /* full source path of the current file  */
    gchar     *to;           /* full destination path (NULL = delete) */
    gint64     file_total;   /* size of the current file              */
    gint64     file_done;
    gint64     total;
    gint64     done;
    gboolean   determinate;
    gboolean   count_items;  /* total/done are item counts, not bytes */
    gboolean   src_remote;   /* conflict dialog: source/destination   */
    gboolean   dst_remote;   /*   paths are remote (no local stat)    */
    gint64     start_time;

    /* UI (main thread only) – Total Commander layout:
     *   From: …   To: …   [file bar]   [total bar]   detail   [Cancel] */
    GtkWidget *window, *phase_lbl, *from_key, *from_lbl, *to_key, *to_lbl;
    GtkWidget *file_bar, *bar, *detail_lbl;
    guint      timer_id;
    gint64     created;      /* job_run start: delayed window reveal  */
    GMainLoop *loop;
    JobFunc    fn;
    gpointer   data;
    /* background (local operations only – SFTP jobs hold the session) */
    gboolean   allow_background; /* set by the caller before job_run    */
    gboolean   background;       /* window hidden, progress in status   */
    gboolean   worker_done;
    GtkWidget *bg_btn;
};

Job     *job_new(FM *fm, const char *title);
void     job_run(Job *job, JobFunc fn, gpointer data);   /* blocks (nested loop) */
void     job_free(Job *job);

/* worker-side setters (thread-safe) */
void     job_set_phase(Job *job, const char *phase);
void     job_set_file (Job *job, const char *file);
void     job_set_total(Job *job, gint64 total);         /* → determinate (bytes) */
void     job_set_total_items(Job *job, gint64 n);       /* → determinate (items) */
void     job_add_done(Job *job, gint64 bytes);
gboolean job_cancelled(Job *job);

/* Background jobs: the worker threads run one after another (a queue);
 * while one runs in the background the main window stays usable. */
guint    job_running_count(void);        /* jobs started and not finished */
gboolean job_foreground_running(void);   /* … one of them with its window shown */
void     job_show_background(FM *fm);    /* Ctrl+J – show their windows   */

#endif /* FM_JOB_H */
