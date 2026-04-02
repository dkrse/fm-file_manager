#include "fm.h"

G_DEFINE_TYPE(FileItem, file_item, G_TYPE_OBJECT)

static void file_item_finalize(GObject *obj)
{
    FileItem *self = FILE_ITEM(obj);
    g_free(self->fg_color);
    g_free(self->name);
    g_free(self->size);
    g_free(self->date);
    g_free(self->dir_path);
    G_OBJECT_CLASS(file_item_parent_class)->finalize(obj);
}

static void file_item_class_init(FileItemClass *klass)
{
    G_OBJECT_CLASS(klass)->finalize = file_item_finalize;
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
