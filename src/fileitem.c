#include "fm.h"

G_DEFINE_TYPE(FileItem, file_item, G_TYPE_OBJECT)

static guint mark_changed_sig;

static void file_item_finalize(GObject *obj)
{
    FileItem *self = FILE_ITEM(obj);
    g_free(self->fg_color);
    g_free(self->name);
    g_free(self->size);
    g_free(self->date);
    g_free(self->dir_path);
    g_free(self->snippet);
    g_free(self->sort_key);
    G_OBJECT_CLASS(file_item_parent_class)->finalize(obj);
}

static void file_item_class_init(FileItemClass *klass)
{
    G_OBJECT_CLASS(klass)->finalize = file_item_finalize;
    mark_changed_sig = g_signal_new("mark-changed", G_TYPE_FROM_CLASS(klass),
                                    G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
                                    G_TYPE_NONE, 0);
}

static void file_item_init(FileItem *self)
{
    (void)self;
}

/*
 * file_item_new_take: takes ownership of size and date strings (caller must not free)
 * icon_name and fg_color must be static constants
 */
FileItem *file_item_new_take(const char *icon_name, const char *name,
                             gchar *size, gchar *date,
                             gboolean is_dir, const char *fg_color,
                             gint weight, gint64 size_raw, gint64 date_raw)
{
    FileItem *item = g_object_new(FILE_ITEM_TYPE, NULL);
    item->icon_name = (gchar *)icon_name;
    item->name      = g_strdup(name);
    item->size      = size;   /* takes ownership */
    item->date      = date;   /* takes ownership */
    item->is_dir    = is_dir;
    item->fg_color  = g_strdup(fg_color);
    item->weight    = weight;
    item->size_raw  = size_raw;
    item->date_raw  = date_raw;
    return item;
}

/* Legacy wrapper that copies strings (for compatibility) */
FileItem *file_item_new(const char *icon_name, const char *name,
                        const char *size, const char *date,
                        gboolean is_dir, const char *fg_color,
                        gint weight, gint64 size_raw, gint64 date_raw)
{
    return file_item_new_take(icon_name, name,
                              g_strdup(size), g_strdup(date),
                              is_dir, fg_color, weight, size_raw, date_raw);
}

const gchar *file_item_sort_key(FileItem *item)
{
    /* g_utf8_collate() normalises both strings on every comparison –
     * ~12x slower than strcmp() on a precomputed key for 50k names.
     * The _for_filename variant also orders "file2" before "file10". */
    if (!item->sort_key)
        item->sort_key = g_utf8_collate_key_for_filename(item->name, -1);
    return item->sort_key;
}

void file_item_set_marked(FileItem *item, gboolean marked)
{
    if (item->marked == marked) return;
    item->marked = marked;
    g_signal_emit(item, mark_changed_sig, 0);
}

/* Independent duplicate (marks are per panel, so the copy is unmarked) */
FileItem *file_item_copy(FileItem *src)
{
    FileItem *it = file_item_new(src->icon_name, src->name, src->size, src->date,
                                 src->is_dir, src->fg_color, src->weight,
                                 src->size_raw, src->date_raw);
    it->dir_path = g_strdup(src->dir_path);
    it->snippet  = g_strdup(src->snippet);
    it->line     = src->line;
    return it;
}
