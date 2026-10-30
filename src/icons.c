#include "fm.h"
#include <string.h>

/* ─────────────────────────────────────────────────────────────────── *
 *  Icon name helpers — freedesktop standard content-type icons
 * ─────────────────────────────────────────────────────────────────── */

/* Well-known directory names → themed folder icons */
static const char *icon_for_dir(const char *name)
{
    static const struct { const char *name; const char *icon; } dirs[] = {
        { "Desktop",       "folder-desktop"       },
        { "Documents",     "folder-documents"     },
        { "Downloads",     "folder-download"      },
        { "Music",         "folder-music"         },
        { "Pictures",      "folder-pictures"      },
        { "Videos",        "folder-videos"        },
        { "Templates",     "folder-templates"     },
        { "Public",        "folder-publicshare"   },
        { ".git",          "folder-git"           },
        { NULL, NULL }
    };
    if (!name) return "folder";
    for (int i = 0; dirs[i].name; i++)
        if (strcmp(name, dirs[i].name) == 0) return dirs[i].icon;
    return "folder";
}

/* File extension → standard freedesktop icon name */
static const char *icon_for_file(const char *name)
{
    if (!name) return "text-x-generic";

    const char *dot = strrchr(name, '.');
    if (!dot || dot == name) {
        /* No extension — check special filenames */
        if (g_str_has_prefix(name, "Makefile") ||
            g_str_has_prefix(name, "CMakeLists") ||
            g_str_has_prefix(name, "Dockerfile") ||
            g_str_has_prefix(name, "Vagrantfile"))
            return "text-x-script";
        if (strcmp(name, "LICENSE") == 0 || strcmp(name, "COPYING") == 0)
            return "text-x-copying";
        if (strcmp(name, "README") == 0)
            return "text-x-readme";
        return "text-x-generic";
    }

    const char *ext = dot + 1;

    /* Images */
    if (!g_ascii_strcasecmp(ext, "png")  || !g_ascii_strcasecmp(ext, "jpg") ||
        !g_ascii_strcasecmp(ext, "jpeg") || !g_ascii_strcasecmp(ext, "gif") ||
        !g_ascii_strcasecmp(ext, "bmp")  || !g_ascii_strcasecmp(ext, "svg") ||
        !g_ascii_strcasecmp(ext, "webp") || !g_ascii_strcasecmp(ext, "ico") ||
        !g_ascii_strcasecmp(ext, "tiff") || !g_ascii_strcasecmp(ext, "tif") ||
        !g_ascii_strcasecmp(ext, "raw")  || !g_ascii_strcasecmp(ext, "psd"))
        return "image-x-generic";

    /* Audio */
    if (!g_ascii_strcasecmp(ext, "mp3")  || !g_ascii_strcasecmp(ext, "ogg") ||
        !g_ascii_strcasecmp(ext, "flac") || !g_ascii_strcasecmp(ext, "wav") ||
        !g_ascii_strcasecmp(ext, "aac")  || !g_ascii_strcasecmp(ext, "wma") ||
        !g_ascii_strcasecmp(ext, "m4a")  || !g_ascii_strcasecmp(ext, "opus"))
        return "audio-x-generic";

    /* Video */
    if (!g_ascii_strcasecmp(ext, "mp4")  || !g_ascii_strcasecmp(ext, "mkv") ||
        !g_ascii_strcasecmp(ext, "avi")  || !g_ascii_strcasecmp(ext, "mov") ||
        !g_ascii_strcasecmp(ext, "wmv")  || !g_ascii_strcasecmp(ext, "flv") ||
        !g_ascii_strcasecmp(ext, "webm") || !g_ascii_strcasecmp(ext, "m4v"))
        return "video-x-generic";

    /* Archives */
    if (!g_ascii_strcasecmp(ext, "zip")  || !g_ascii_strcasecmp(ext, "tar") ||
        !g_ascii_strcasecmp(ext, "gz")   || !g_ascii_strcasecmp(ext, "bz2") ||
        !g_ascii_strcasecmp(ext, "xz")   || !g_ascii_strcasecmp(ext, "7z")  ||
        !g_ascii_strcasecmp(ext, "rar")  || !g_ascii_strcasecmp(ext, "zst") ||
        !g_ascii_strcasecmp(ext, "tgz")  || !g_ascii_strcasecmp(ext, "tbz2") ||
        !g_ascii_strcasecmp(ext, "txz")  || !g_ascii_strcasecmp(ext, "deb") ||
        !g_ascii_strcasecmp(ext, "rpm"))
        return "package-x-generic";

    /* PDF */
    if (!g_ascii_strcasecmp(ext, "pdf"))
        return "application-pdf";

    /* Office documents */
    if (!g_ascii_strcasecmp(ext, "doc")  || !g_ascii_strcasecmp(ext, "docx") ||
        !g_ascii_strcasecmp(ext, "odt")  || !g_ascii_strcasecmp(ext, "rtf"))
        return "x-office-document";
    if (!g_ascii_strcasecmp(ext, "xls")  || !g_ascii_strcasecmp(ext, "xlsx") ||
        !g_ascii_strcasecmp(ext, "ods")  || !g_ascii_strcasecmp(ext, "csv"))
        return "x-office-spreadsheet";
    if (!g_ascii_strcasecmp(ext, "ppt")  || !g_ascii_strcasecmp(ext, "pptx") ||
        !g_ascii_strcasecmp(ext, "odp"))
        return "x-office-presentation";

    /* Web / markup */
    if (!g_ascii_strcasecmp(ext, "html") || !g_ascii_strcasecmp(ext, "htm") ||
        !g_ascii_strcasecmp(ext, "xhtml"))
        return "text-html";
    if (!g_ascii_strcasecmp(ext, "css"))
        return "text-css";
    if (!g_ascii_strcasecmp(ext, "xml")  || !g_ascii_strcasecmp(ext, "xsl") ||
        !g_ascii_strcasecmp(ext, "xslt"))
        return "text-xml";
    if (!g_ascii_strcasecmp(ext, "json"))
        return "application-json";

    /* Source code */
    if (!g_ascii_strcasecmp(ext, "c")    || !g_ascii_strcasecmp(ext, "h") ||
        !g_ascii_strcasecmp(ext, "cpp")  || !g_ascii_strcasecmp(ext, "cc") ||
        !g_ascii_strcasecmp(ext, "cxx")  || !g_ascii_strcasecmp(ext, "hpp"))
        return "text-x-csrc";
    if (!g_ascii_strcasecmp(ext, "py")   || !g_ascii_strcasecmp(ext, "pyw"))
        return "text-x-python";
    if (!g_ascii_strcasecmp(ext, "java")  || !g_ascii_strcasecmp(ext, "class") ||
        !g_ascii_strcasecmp(ext, "jar"))
        return "application-x-java";
    if (!g_ascii_strcasecmp(ext, "js")   || !g_ascii_strcasecmp(ext, "mjs") ||
        !g_ascii_strcasecmp(ext, "ts")   || !g_ascii_strcasecmp(ext, "jsx") ||
        !g_ascii_strcasecmp(ext, "tsx"))
        return "application-javascript";
    if (!g_ascii_strcasecmp(ext, "rs"))
        return "text-x-rust";
    if (!g_ascii_strcasecmp(ext, "go"))
        return "text-x-go";
    if (!g_ascii_strcasecmp(ext, "rb"))
        return "application-x-ruby";
    if (!g_ascii_strcasecmp(ext, "php"))
        return "application-x-php";
    if (!g_ascii_strcasecmp(ext, "sql"))
        return "text-x-sql";

    /* Shell / scripts */
    if (!g_ascii_strcasecmp(ext, "sh")   || !g_ascii_strcasecmp(ext, "bash") ||
        !g_ascii_strcasecmp(ext, "zsh")  || !g_ascii_strcasecmp(ext, "fish") ||
        !g_ascii_strcasecmp(ext, "pl")   || !g_ascii_strcasecmp(ext, "lua"))
        return "text-x-script";

    /* Config / data */
    if (!g_ascii_strcasecmp(ext, "conf") || !g_ascii_strcasecmp(ext, "cfg") ||
        !g_ascii_strcasecmp(ext, "ini")  || !g_ascii_strcasecmp(ext, "toml") ||
        !g_ascii_strcasecmp(ext, "yaml") || !g_ascii_strcasecmp(ext, "yml"))
        return "text-x-generic";

    /* Markdown / docs */
    if (!g_ascii_strcasecmp(ext, "md")   || !g_ascii_strcasecmp(ext, "rst") ||
        !g_ascii_strcasecmp(ext, "txt")  || !g_ascii_strcasecmp(ext, "log"))
        return "text-x-generic";

    /* Fonts */
    if (!g_ascii_strcasecmp(ext, "ttf")  || !g_ascii_strcasecmp(ext, "otf") ||
        !g_ascii_strcasecmp(ext, "woff") || !g_ascii_strcasecmp(ext, "woff2"))
        return "font-x-generic";

    /* Shared libraries */
    if (!g_ascii_strcasecmp(ext, "so")   || !g_ascii_strcasecmp(ext, "dll") ||
        !g_ascii_strcasecmp(ext, "dylib"))
        return "application-x-sharedlib";

    /* Disk images */
    if (!g_ascii_strcasecmp(ext, "iso")  || !g_ascii_strcasecmp(ext, "img"))
        return "application-x-cd-image";

    /* Database */
    if (!g_ascii_strcasecmp(ext, "db")   || !g_ascii_strcasecmp(ext, "sqlite") ||
        !g_ascii_strcasecmp(ext, "sqlite3"))
        return "application-vnd.oasis.opendocument.database";

    return "text-x-generic";
}

/* Combined: pick icon based on entry type + name */
const char *icon_for_entry(const char *name, gboolean is_dir,
                           gboolean is_link, gboolean is_exec)
{
    if (is_link) return "emblem-symbolic-link";
    if (is_dir)  return icon_for_dir(name);
    if (is_exec) return "application-x-executable";
    return icon_for_file(name);
}
