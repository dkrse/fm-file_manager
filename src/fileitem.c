#include "fm.h"

G_DEFINE_TYPE(FileItem, file_item, G_TYPE_OBJECT)

static void file_item_finalize(GObject *obj)
{
    FileItem *self = FILE_ITEM(obj);
    g_free(self->icon_name);
    g_free(self->name);
    g_free(self->size);
    g_free(self->date);
    g_free(self->fg_color);
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

FileItem *file_item_new(const char *icon_name, const char *name,
                        const char *size, const char *date,
                        gboolean is_dir, const char *fg_color,
                        gint weight, gint64 size_raw, gint64 date_raw)
{
    FileItem *item = g_object_new(FILE_ITEM_TYPE, NULL);
    item->icon_name = g_strdup(icon_name);
    item->name      = g_strdup(name);
    item->size      = g_strdup(size);
    item->date      = g_strdup(date);
    item->is_dir    = is_dir;
    item->fg_color  = g_strdup(fg_color);
    item->weight    = weight;
    item->size_raw  = size_raw;
    item->date_raw  = date_raw;
    return item;
}
