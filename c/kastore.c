#ifdef __unix__
#define _POSIX_C_SOURCE 1
#include <sys/mman.h>
#include <sys/stat.h>
#endif

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <assert.h>
#include <errno.h>

#include "kastore.h"

const char *
kas_strerror(int err)
{
    const char *ret = "Unknown error";

    switch (err) {
        case KAS_ERR_GENERIC:
            ret = "Generic error; please file a bug report";
            break;
        case KAS_ERR_IO:
            if (errno != 0) {
                ret = strerror(errno);
            }  else {
                ret = "I/O error with errno unset. Please file a bug report";
            }
            break;
        case KAS_ERR_BAD_MODE:
            ret = "Bad open mode; must be \"r\" or \"w\"";
            break;
        case KAS_ERR_NO_MEMORY:
            ret = "Out of memory";
            break;
        case KAS_ERR_BAD_FILE_FORMAT:
            ret = "File not in KAS format";
            break;
        case KAS_ERR_VERSION_TOO_OLD:
            ret = "File format version is too old. Please upgrade using "
                "'kas upgrade <filename>'";
            break;
        case KAS_ERR_VERSION_TOO_NEW:
            ret = "File format version is too new. Please upgrade your "
                "kastore library version";
            break;
        case KAS_ERR_BAD_TYPE:
            ret = "Unknown data type";
            break;
        case KAS_ERR_DUPLICATE_KEY:
            ret = "Duplicate key provided";
            break;
        case KAS_ERR_KEY_NOT_FOUND:
            ret = "Key not found";
            break;
    }
    return ret;
}

static size_t
type_size(int type)
{
    const size_t type_size_map[] = {1, 1, 4, 4, 8, 8, 4, 8};
    assert(type < KAS_NUM_TYPES);
    return type_size_map[type];
}

/* Compare item keys lexicographically. */
static int
compare_items(const void *a, const void *b) {
    const kaitem_t *ia = (const kaitem_t *) a;
    const kaitem_t *ib = (const kaitem_t *) b;
    size_t len = ia->key_len < ib->key_len? ia->key_len: ib->key_len;
    int ret = memcmp(ia->key, ib->key, len);
    if (ret == 0) {
        ret = (ia->key_len > ib->key_len) - (ia->key_len < ib->key_len);
    }
    return ret;
}

/* When a read error occurs we don't know whether this is because the file
 * ended unexpectedly or an IO error occured. If the file ends unexpectedly
 * this is a file format error.
 */
static int KAS_WARN_UNUSED
kastore_get_read_io_error(kastore_t *self)
{
    int ret = KAS_ERR_IO;

    if (feof(self->file) || errno == 0) {
        ret = KAS_ERR_BAD_FILE_FORMAT;
    }
    return ret;
}

static int KAS_WARN_UNUSED
kastore_write_header(kastore_t *self)
{
    int ret = 0;
    char header[KAS_HEADER_SIZE];
    uint16_t version_major = KAS_FILE_VERSION_MAJOR;
    uint16_t version_minor = KAS_FILE_VERSION_MINOR;
    uint32_t num_items = (uint32_t) self->num_items;
    uint64_t file_size = (uint64_t) self->file_size;

    memset(header, 0, sizeof(header));
    memcpy(header, KAS_MAGIC, 8);
    memcpy(header + 8, &version_major, 2);
    memcpy(header + 10, &version_minor, 2);
    memcpy(header + 12, &num_items, 4);
    memcpy(header + 16, &file_size, 8);
    /* Rest of header is reserved */
    if (fwrite(header, KAS_HEADER_SIZE, 1, self->file) != 1) {
        ret = KAS_ERR_IO;
        goto out;
    }
out:
    return ret;
}

static int KAS_WARN_UNUSED
kastore_read_header(kastore_t *self)
{
    int ret = 0;
    char header[KAS_HEADER_SIZE];
    uint16_t version_major, version_minor;
    uint32_t num_items;
    uint64_t file_size;
    size_t count;

    count = fread(header, KAS_HEADER_SIZE, 1, self->file);
    if (count == 0) {
        ret = kastore_get_read_io_error(self);
        goto out;
    }
    if (strncmp(header, KAS_MAGIC, 8) != 0) {
        ret = KAS_ERR_BAD_FILE_FORMAT;
        goto out;
    }
    memcpy(&version_major, header + 8, 2);
    memcpy(&version_minor, header + 10, 2);
    memcpy(&num_items, header + 12, 4);
    memcpy(&file_size, header + 16, 8);
    self->file_version[0] = (int) version_major;
    self->file_version[1] = (int) version_minor;
    if (self->file_version[0] < KAS_FILE_VERSION_MAJOR) {
        ret = KAS_ERR_VERSION_TOO_OLD;
        goto out;
    } else if (self->file_version[0] > KAS_FILE_VERSION_MAJOR) {
        ret = KAS_ERR_VERSION_TOO_NEW;
        goto out;
    }
    self->num_items = num_items;
    self->file_size = file_size;
    if (self->file_size < KAS_HEADER_SIZE) {
        ret = KAS_ERR_BAD_FILE_FORMAT;
        goto out;
    }
out:
    return ret;
}

/* Compute the locations of the keys and arrays in the file. */
static int KAS_WARN_UNUSED
kastore_pack_items(kastore_t *self)
{
    int ret = 0;
    size_t j, offset, remainder;

    /* Pack the keys */
    offset = KAS_HEADER_SIZE + self->num_items * KAS_ITEM_DESCRIPTOR_SIZE;
    for (j = 0; j < self->num_items; j++) {
        self->items[j].key_start = offset;
        offset += self->items[j].key_len;
    }
    /* Pack the arrays */
    for (j = 0; j < self->num_items; j++) {
        remainder = offset % KAS_ARRAY_ALIGN;
        if (remainder != 0) {
            offset += KAS_ARRAY_ALIGN - remainder;
        }
        self->items[j].array_start = offset;
        offset += self->items[j].array_len * type_size(self->items[j].type);
    }
    self->file_size = offset;
    return ret;
}

static int KAS_WARN_UNUSED
kastore_write_descriptors(kastore_t *self)
{
    int ret = 0;
    size_t j;
    uint8_t type;
    uint64_t key_start, key_len, array_start, array_len;
    char descriptor[KAS_ITEM_DESCRIPTOR_SIZE];

    for (j = 0; j < self->num_items; j++) {
        memset(descriptor, 0, KAS_ITEM_DESCRIPTOR_SIZE);
        type = (uint8_t) self->items[j].type;
        key_start = (uint64_t) self->items[j].key_start;
        key_len = (uint64_t) self->items[j].key_len;
        array_start = (uint64_t) self->items[j].array_start;
        array_len = (uint64_t) self->items[j].array_len;
        memcpy(descriptor, &type, 1);
        /* Bytes 1-8 are reserved */
        memcpy(descriptor + 8, &key_start, 8);
        memcpy(descriptor + 16, &key_len, 8);
        memcpy(descriptor + 24, &array_start, 8);
        memcpy(descriptor + 32, &array_len, 8);
        /* Rest of descriptor is reserved */
        if (fwrite(descriptor, sizeof(descriptor), 1, self->file) != 1) {
            ret = KAS_ERR_IO;
            goto out;
        }
    }
out:
    return ret;
}

static int KAS_WARN_UNUSED
kastore_read_descriptors(kastore_t *self)
{
    int ret = KAS_ERR_BAD_FILE_FORMAT;
    size_t j;
    uint8_t type;
    uint64_t key_start, key_len, array_start, array_len;
    char *descriptor;
    size_t descriptor_offset, offset, remainder;

    descriptor_offset = KAS_HEADER_SIZE;
    if (descriptor_offset + self->num_items * KAS_ITEM_DESCRIPTOR_SIZE
            > self->file_size) {
        goto out;
    }
    for (j = 0; j < self->num_items; j++) {
        descriptor = self->read_buffer + descriptor_offset;
        descriptor_offset += KAS_ITEM_DESCRIPTOR_SIZE;
        memcpy(&type, descriptor, 1);
        memcpy(&key_start, descriptor + 8, 8);
        memcpy(&key_len, descriptor + 16, 8);
        memcpy(&array_start, descriptor + 24, 8);
        memcpy(&array_len, descriptor + 32, 8);

        if (type >= KAS_NUM_TYPES) {
            ret = KAS_ERR_BAD_TYPE;
            goto out;
        }
        self->items[j].type = (int) type;
        if (key_start + key_len > self->file_size) {
            goto out;
        }
        self->items[j].key_start = (size_t) key_start;
        self->items[j].key_len = (size_t) key_len;
        self->items[j].key = self->read_buffer + key_start;
        if (array_start + array_len * type_size(type) > self->file_size) {
            goto out;
        }
        self->items[j].array_start = (size_t) array_start;
        self->items[j].array_len = (size_t) array_len;
        self->items[j].array = self->read_buffer + array_start;
    }

    /* Check the integrity of the key and array packing. Keys must
     * be packed sequentially starting immediately after the descriptors. */
    offset = KAS_HEADER_SIZE + self->num_items * KAS_ITEM_DESCRIPTOR_SIZE;
    for (j = 0; j < self->num_items; j++) {
        if (self->items[j].key_start != offset) {
            ret = KAS_ERR_BAD_FILE_FORMAT;
            goto out;
        }
        offset += self->items[j].key_len;
    }
    for (j = 0; j < self->num_items; j++) {
        /* Arrays are 8 byte aligned and adjacent */
        remainder = offset % KAS_ARRAY_ALIGN;
        if (remainder != 0) {
            offset += KAS_ARRAY_ALIGN - remainder;
        }
        if (self->items[j].array_start != offset) {
            ret = KAS_ERR_BAD_FILE_FORMAT;
            goto out;
        }
        offset += self->items[j].array_len * type_size(self->items[j].type);
    }

    ret = 0;
out:
    return ret;
}

static int KAS_WARN_UNUSED
kastore_write_data(kastore_t *self)
{
    int ret = 0;
    size_t j, size, offset, padding;
    char pad[KAS_ARRAY_ALIGN] = {0, 0, 0, 0, 0, 0, 0};

    offset = KAS_HEADER_SIZE + self->num_items * KAS_ITEM_DESCRIPTOR_SIZE;

    /* Write the keys. */
    for (j = 0; j < self->num_items; j++) {
        assert(ftell(self->file) == (long) self->items[j].key_start);
        assert(offset == self->items[j].key_start);
        if (fwrite(self->items[j].key, self->items[j].key_len, 1, self->file) != 1) {
            ret = KAS_ERR_IO;
            goto out;
        }
        offset += self->items[j].key_len;
    }
    /* Write the arrays. */
    for (j = 0; j < self->num_items; j++) {
        padding = self->items[j].array_start - offset;
        assert(padding < KAS_ARRAY_ALIGN);
        if (padding > 0 && fwrite(pad, padding, 1, self->file) != 1) {
            ret = KAS_ERR_IO;
            goto out;
        }
        assert(ftell(self->file) == (long) self->items[j].array_start);
        size = self->items[j].array_len * type_size(self->items[j].type);
        if (size > 0 && fwrite(self->items[j].array, size, 1, self->file) != 1) {
            ret = KAS_ERR_IO;
            goto out;
        }
        offset = self->items[j].array_start + size;
    }
out:
    return ret;
}

#if __unix__
static int KAS_WARN_UNUSED
kastore_mmap_file(kastore_t *self)
{
    int ret = 0;
    void *p;
    struct stat st;

    if (stat(self->filename, &st) != 0) {
        ret = KAS_ERR_IO;
        goto out;
    }
    if (self->file_size != (size_t) st.st_size) {
        ret = KAS_ERR_BAD_FILE_FORMAT;
        goto out;
    }
    p = mmap(NULL, self->file_size, PROT_READ,  MAP_PRIVATE,
            fileno(self->file), 0);
    if (p == MAP_FAILED) {
        ret = KAS_ERR_IO;
        goto out;
    }
    self->read_buffer = p;
out:
    return ret;
}

static void
kastore_munmap_file(kastore_t *self)
{
    if (self->read_buffer != NULL) {
        munmap(self->read_buffer, self->file_size);
    }
}
#endif

static int KAS_WARN_UNUSED
kastore_read_file(kastore_t *self)
{
    int ret = 0;
    int err;
    size_t count;

    self->read_buffer = malloc(self->file_size);
    if (self->read_buffer == NULL) {
        ret = KAS_ERR_NO_MEMORY;
        goto out;
    }
    err = fseek(self->file, 0, SEEK_SET);
    if (err != 0) {
        ret = KAS_ERR_IO;
        goto out;
    }
    count = fread(self->read_buffer, self->file_size, 1, self->file);
    if (count == 0) {
        ret = kastore_get_read_io_error(self);
        goto out;
    }
out:
    return ret;
}

static int KAS_WARN_UNUSED
kastore_write_file(kastore_t *self)
{
    int ret = 0;

    qsort(self->items, self->num_items, sizeof(kaitem_t), compare_items);
    ret = kastore_pack_items(self);
    if (ret != 0) {
        goto out;
    }
    ret = kastore_write_header(self);
    if (ret != 0) {
        goto out;
    }
    ret = kastore_write_descriptors(self);
    if (ret != 0) {
        goto out;
    }
    ret = kastore_write_data(self);
    if (ret != 0) {
        goto out;
    }
out:
    return ret;
}

static int KAS_WARN_UNUSED
kastore_read(kastore_t *self)
{
    int ret = 0;

    ret = kastore_read_header(self);
    if (ret != 0) {
        goto out;
    }
#ifdef __unix__
    if (!self->flags & KAS_NO_MMAP) {
        ret = kastore_mmap_file(self);
        if (ret != 0) {
            goto out;
        }
#else
    /* On windows we silently ignore MMAP */
    if (0) {
#endif
    } else {
        ret = kastore_read_file(self);
        if (ret != 0) {
            goto out;
        }
    }

    if (self->num_items > 0) {
        self->items = calloc(self->num_items, sizeof(*self->items));
        if (self->items == NULL) {
            ret = KAS_ERR_NO_MEMORY;
            goto out;
        }
        ret = kastore_read_descriptors(self);
        if (ret != 0) {
            goto out;
        }
    }
out:
    return ret;
}

int KAS_WARN_UNUSED
kastore_open(kastore_t *self, const char *filename, const char *mode, int flags)
{
    int ret = 0;
    const char *file_mode;

    memset(self, 0, sizeof(*self));
    if (strlen(mode) != 1) {
        ret = KAS_ERR_BAD_MODE;
        goto out;
    }
    if (strncmp(mode, "r", 1) == 0) {
        self->mode = KAS_READ;
        file_mode = "rb";
    } else if (strncmp(mode, "w", 1) == 0) {
        self->mode = KAS_WRITE;
        file_mode = "wb";
    } else {
        ret = KAS_ERR_BAD_MODE;
        goto out;
    }
    self->flags = flags;
    self->filename = filename;
    self->file = fopen(filename, file_mode);
    if (self->file == NULL) {
        ret = KAS_ERR_IO;
        goto out;
    }
    if (self->mode == KAS_READ) {
        ret = kastore_read(self);
    }
out:
    return ret;
}

int KAS_WARN_UNUSED
kastore_close(kastore_t *self)
{
    int ret = 0;
    int err;
    size_t j;

    if (self->mode == KAS_WRITE) {
        if (self->file != NULL) {
            ret = kastore_write_file(self);
            if (ret != 0) {
                /* Ignore errors on close now */
                fclose(self->file);
                self->file = NULL;
            }
        }
        if (self->items != NULL) {
            /* We only alloc memory for the keys in write mode */
            for (j = 0; j < self->num_items; j++) {
                kas_safe_free(self->items[j].key);
            }
        }
    }
    kas_safe_free(self->items);
#if __unix__
    if (! (self->flags & KAS_NO_MMAP)) {
        kastore_munmap_file(self);
    } else {
        kas_safe_free(self->read_buffer);
    }
#else
    /* On windows we must have alloced the read buffer. */
    kas_safe_free(self->read_buffer);
#endif
    if (self->file != NULL) {
        err = fclose(self->file);
        if (err != 0) {
            ret = KAS_ERR_IO;
        }
    }
    memset(self, 0, sizeof(*self));
    return ret;
}

int KAS_WARN_UNUSED
kastore_get(kastore_t *self, const char *key, size_t key_len,
        const void **array, size_t *array_len, int *type)
{
    int ret = KAS_ERR_KEY_NOT_FOUND;
    kaitem_t search;
    kaitem_t *item;
    search.key = malloc(key_len);
    search.key_len = key_len;

    if (search.key == NULL) {
        ret = KAS_ERR_NO_MEMORY;
        goto out;
    }
    memcpy(search.key, key, key_len);
    item = bsearch(&search, self->items, self->num_items, sizeof(kaitem_t),
            compare_items);
    if (item == NULL) {
        goto out;
    }
    *array = item->array;
    *array_len = item->array_len;
    *type = item->type;
    ret = 0;
out:
    kas_safe_free(search.key);
    return ret;
}

int KAS_WARN_UNUSED
kastore_put(kastore_t *self, const char *key, size_t key_len,
       const void *array, size_t array_len, int type,
       int KAS_UNUSED(flags))
{
    int ret = 0;
    kaitem_t *new_item;
    void *p;
    size_t j;

    if (type < 0 || type >= KAS_NUM_TYPES) {
        ret = KAS_ERR_BAD_TYPE;
        goto out;
    }
    if (key_len == 0) {
        ret = KAS_ERR_EMPTY_KEY;
        goto out;
    }
    /* This isn't terribly efficient, but we're not expecting large
     * numbers of items. */
    p = realloc(self->items, (self->num_items + 1) * sizeof(*self->items));
    if (p == NULL) {
        ret = KAS_ERR_NO_MEMORY;
        goto out;
    }
    self->items = p;
    new_item = self->items + self->num_items;
    self->num_items++;
    memset(new_item, 0, sizeof(*new_item));
    new_item->type = type;
    new_item->key = malloc(key_len);
    if (new_item->key == NULL) {
        ret = KAS_ERR_NO_MEMORY;
        goto out;
    }
    memcpy(new_item->key, key, key_len);
    new_item->key_len = key_len;
    new_item->array = array;
    new_item->array_len = array_len;

    /* Check if this key is already in here. OK, this is a quadratic time
     * algorithm, but we're not expecting to have lots of items (< 100). In
     * this case, the simple algorithm is probably better. If/when we ever
     * deal with more items than this, then we will need a better algorithm.
     */
    for (j = 0; j < self->num_items - 1; j++) {
        if (compare_items(new_item, self->items + j) == 0) {
            /* Free the key memory and remove this item */
            self->num_items--;
            free(new_item->key);
            ret = KAS_ERR_DUPLICATE_KEY;
            goto out;
        }
    }
out:
    return ret;
}

void
kastore_print_state(kastore_t *self, FILE *out)
{
    kaitem_t *item;
    size_t j;

    fprintf(out, "============================\n");
    fprintf(out, "kastore state\n");
    fprintf(out, "file_version = %d.%d\n", self->file_version[0], self->file_version[1]);
    fprintf(out, "mode = %d\n", self->mode);
    fprintf(out, "num_items = %zu\n", self->num_items);
    fprintf(out, "file_size = %zu\n", self->file_size);
    fprintf(out, "filename = '%s'\n", self->filename);
    fprintf(out, "file = '%p'\n", (void *) self->file);
    fprintf(out, "============================\n");
    for (j = 0; j < self->num_items; j++) {
        item = self->items + j;
        fprintf(out, "%.*s: type=%d, key_start=%zu, key_len=%zu, array_start=%zu, array_len=%zu\n",
                (int) item->key_len, item->key, item->type, item->key_start, item->key_len,
                item->array_start, item->array_len);

    }
    fprintf(out, "============================\n");
}
