/* Private strict JSON lexical engine shared by wire and package metadata.
 * Instantiated per translation unit with explicit GT_JSON_* types/budgets.
 * No schema, floating point, locale or external IO. Wire uses its original fixed
 * workspace; package metadata opts into bounded, wiped, demand-grown buffers. */
typedef enum {
    J_OBJECT,
    J_ARRAY,
    J_STRING,
    J_NUMBER,
    J_TRUE,
    J_FALSE,
    J_NULL
} json_kind_t;

typedef struct {
    json_kind_t kind;
    uint16_t child, next, parent, count;
    uint32_t key, key_size, text, size, points, start, bytes;
} json_node_t;

struct GT_JSON_TAG {
    gt_allocator_t allocator;
    const uint8_t *input;
    size_t input_size, at, strings_size, key_work;
    uint16_t used;
    bool valid;
    GT_JSON_ERROR error;
#ifdef GT_JSON_DYNAMIC
    json_node_t *nodes;
    uint16_t *keys;
    uint8_t *strings;
    size_t nodes_capacity, keys_capacity, strings_capacity, key_count;
#else
    json_node_t nodes[GT_JSON_NODES + 1];
    uint16_t keys[GT_JSON_KEYS];
    uint8_t strings[GT_JSON_BYTES];
#endif
};

static void *allocate(void *ctx, size_t n) {
    (void)ctx;
    return malloc(n);
}

static void release(void *ctx, void *p, size_t n) {
    (void)ctx;
    (void)n;
    free(p);
}

static bool fail(GT_JSON_DOC *d, GT_JSON_ERROR error) {
    d->error = error;
    return false;
}
#ifdef GT_JSON_DYNAMIC
static bool json_grow(GT_JSON_DOC *d,
                      void **buffer,
                      size_t *capacity,
                      size_t need,
                      size_t maximum,
                      size_t item_size) {
    if (need <= *capacity) {
        return true;
    }
    if (need > maximum) {
        return fail(d, GT_JSON_LIMIT);
    }
    size_t next = *capacity ? *capacity : 64;
    while (next < need) {
        next = next > maximum / 2 ? maximum : next * 2;
    }
    if (next > maximum) {
        next = maximum;
    }
    void *p = gt__plugin_memory_alloc(&d->allocator, next * item_size);
    if (!p) {
        return fail(d, GT_JSON_MEMORY);
    }
    memset(p, 0, next * item_size);
    if (*buffer) {
        memcpy(p, *buffer, *capacity * item_size);
    }
    gt__plugin_memory_free(&d->allocator, *buffer, *capacity * item_size);
    *buffer = p;
    *capacity = next;
    return true;
}

static bool json_nodes(GT_JSON_DOC *d, size_t need) {
    void *p = d->nodes;
    bool ok = json_grow(d, &p, &d->nodes_capacity, need, GT_JSON_NODES + 1, sizeof(*d->nodes));
    d->nodes = p;
    return ok;
}

static bool json_strings(GT_JSON_DOC *d, size_t need) {
    void *p = d->strings;
    bool ok = json_grow(d, &p, &d->strings_capacity, need, GT_JSON_BYTES, 1);
    d->strings = p;
    return ok;
}

static void json_buffers_free(GT_JSON_DOC *d) {
    gt__plugin_memory_free(&d->allocator, d->nodes, d->nodes_capacity * sizeof(*d->nodes));
    gt__plugin_memory_free(&d->allocator, d->keys, d->keys_capacity * sizeof(*d->keys));
    gt__plugin_memory_free(&d->allocator, d->strings, d->strings_capacity);
    d->nodes = NULL;
    d->keys = NULL;
    d->strings = NULL;
    d->nodes_capacity = d->keys_capacity = d->strings_capacity = d->key_count = 0;
}
#endif
static bool digit(uint8_t c) {
    return c >= '0' && c <= '9';
}

static void space(GT_JSON_DOC *d) {
    while (d->at < d->input_size) {
        uint8_t c = d->input[d->at];
        if (c != ' ' && c != '\t' && c != '\r' && c != '\n') {
            break;
        }
        d->at++;
    }
}

static bool append_scalar(GT_JSON_DOC *d, uint32_t cp) {
    uint8_t out[4];
    size_t n;
    if (cp < 0x80) {
        out[0] = (uint8_t)cp;
        n = 1;
    } else if (cp < 0x800) {
        out[0] = (uint8_t)(0xc0 | (cp >> 6));
        out[1] = (uint8_t)(0x80 | (cp & 63));
        n = 2;
    } else if (cp < 0x10000) {
        out[0] = (uint8_t)(0xe0 | (cp >> 12));
        out[1] = (uint8_t)(0x80 | ((cp >> 6) & 63));
        out[2] = (uint8_t)(0x80 | (cp & 63));
        n = 3;
    } else {
        out[0] = (uint8_t)(0xf0 | (cp >> 18));
        out[1] = (uint8_t)(0x80 | ((cp >> 12) & 63));
        out[2] = (uint8_t)(0x80 | ((cp >> 6) & 63));
        out[3] = (uint8_t)(0x80 | (cp & 63));
        n = 4;
    }
    if (n > GT_JSON_BYTES - d->strings_size) {
        return fail(d, GT_JSON_LIMIT);
    }
#ifdef GT_JSON_DYNAMIC
    if (!json_strings(d, d->strings_size + n)) {
        return false;
    }
#endif
    memcpy(d->strings + d->strings_size, out, n);
    d->strings_size += n;
    return true;
}

static bool hex4(GT_JSON_DOC *d, uint32_t *out) {
    if (d->input_size - d->at < 4) {
        return fail(d, GT_JSON_SYNTAX);
    }
    uint32_t value = 0;
    for (unsigned i = 0; i < 4; i++) {
        uint8_t c = d->input[d->at++];
        unsigned v;
        if (digit(c)) {
            v = c - '0';
        } else if (c >= 'a' && c <= 'f') {
            v = c - 'a' + 10;
        } else if (c >= 'A' && c <= 'F') {
            v = c - 'A' + 10;
        } else {
            return fail(d, GT_JSON_SYNTAX);
        }
        value = (value << 4) | v;
    }
    *out = value;
    return true;
}

static bool string(GT_JSON_DOC *d, uint32_t *offset, uint32_t *size, uint32_t *points) {
    if (d->at >= d->input_size || d->input[d->at++] != '"') {
        return fail(d, GT_JSON_SYNTAX);
    }
#ifdef GT_JSON_DYNAMIC
    if (!json_strings(d, d->strings_size + 1)) {
        return false;
    }
#endif
    *offset = (uint32_t)d->strings_size;
    *points = 0;
    while (d->at < d->input_size) {
        uint32_t cp = d->input[d->at++];
        if (cp == '"') {
            *size = (uint32_t)d->strings_size - *offset;
            return true;
        }
        if (cp < 32) {
            return fail(d, GT_JSON_SYNTAX);
        }
        if (cp == '\\') {
            if (d->at == d->input_size) {
                return fail(d, GT_JSON_SYNTAX);
            }
            cp = d->input[d->at++];
            switch (cp) {
                case '"':
                case '\\':
                case '/':
                    break;
                case 'b':
                    cp = 8;
                    break;
                case 'f':
                    cp = 12;
                    break;
                case 'n':
                    cp = 10;
                    break;
                case 'r':
                    cp = 13;
                    break;
                case 't':
                    cp = 9;
                    break;
                case 'u': {
                    if (!hex4(d, &cp)) {
                        return false;
                    }
                    if (cp >= 0xd800 && cp <= 0xdbff) {
                        if (d->input_size - d->at < 2 || d->input[d->at] != '\\' ||
                            d->input[d->at + 1] != 'u') {
                            return fail(d, GT_JSON_UTF8);
                        }
                        d->at += 2;
                        uint32_t low;
                        if (!hex4(d, &low)) {
                            return false;
                        }
                        if (low < 0xdc00 || low > 0xdfff) {
                            return fail(d, GT_JSON_UTF8);
                        }
                        cp = 0x10000 + ((cp - 0xd800) << 10) + (low - 0xdc00);
                    } else if (cp >= 0xdc00 && cp <= 0xdfff) {
                        return fail(d, GT_JSON_UTF8);
                    }
                    break;
                }
                default:
                    return fail(d, GT_JSON_SYNTAX);
            }
        } else if (cp >= 0x80) {
            unsigned continuation;
            uint32_t minimum;
            if (cp >= 0xc2 && cp <= 0xdf) {
                continuation = 1;
                minimum = 0x80;
                cp &= 31;
            } else if (cp >= 0xe0 && cp <= 0xef) {
                continuation = 2;
                minimum = 0x800;
                cp &= 15;
            } else if (cp >= 0xf0 && cp <= 0xf4) {
                continuation = 3;
                minimum = 0x10000;
                cp &= 7;
            } else {
                return fail(d, GT_JSON_UTF8);
            }
            if (d->input_size - d->at < continuation) {
                return fail(d, GT_JSON_UTF8);
            }
            for (unsigned i = 0; i < continuation; i++) {
                uint8_t c = d->input[d->at++];
                if ((c & 0xc0) != 0x80) {
                    return fail(d, GT_JSON_UTF8);
                }
                cp = (cp << 6) | (c & 63);
            }
            if (cp < minimum || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff)) {
                return fail(d, GT_JSON_UTF8);
            }
        }
        if (!append_scalar(d, cp)) {
            return false;
        }
        (*points)++;
    }
    return fail(d, GT_JSON_SYNTAX);
}

static uint64_t key_hash(const GT_JSON_DOC *d, uint16_t id) {
    const json_node_t *n = &d->nodes[id];
    uint64_t hash = UINT64_C(14695981039346656037) ^ n->parent;
    for (size_t i = 0; i < n->key_size; i++) {
        hash ^= d->strings[n->key + i];
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

static bool key_insert(GT_JSON_DOC *d, uint16_t id) {
#ifdef GT_JSON_DYNAMIC
    if (!d->keys_capacity || d->key_count >= d->keys_capacity / 2) {
        size_t next = d->keys_capacity ? d->keys_capacity * 2 : 128;
        if (next > GT_JSON_KEYS) {
            return fail(d, GT_JSON_LIMIT);
        }
        uint16_t *keys = gt__plugin_memory_alloc(&d->allocator, next * sizeof(*keys));
        if (!keys) {
            return fail(d, GT_JSON_MEMORY);
        }
        memset(keys, 0, next * sizeof(*keys));
        for (size_t i = 0; i < d->keys_capacity; i++) {
            if (d->keys[i]) {
                size_t slot = (size_t)key_hash(d, d->keys[i]) & (next - 1);
                while (keys[slot]) {
                    if (++d->key_work > GT_JSON_WORK) {
                        gt__plugin_memory_free(&d->allocator, keys, next * sizeof(*keys));
                        return fail(d, GT_JSON_LIMIT);
                    }
                    slot = (slot + 1) & (next - 1);
                }
                keys[slot] = d->keys[i];
            }
        }
        gt__plugin_memory_free(&d->allocator, d->keys, d->keys_capacity * sizeof(*keys));
        d->keys = keys;
        d->keys_capacity = next;
    }
    size_t mask = d->keys_capacity - 1;
#else
    size_t mask = GT_JSON_KEYS - 1;
#endif
    const json_node_t *n = &d->nodes[id];
    size_t slot = (size_t)key_hash(d, id) & mask;
    for (;; slot = (slot + 1) & mask) {
        if (++d->key_work > GT_JSON_WORK) {
            return fail(d, GT_JSON_LIMIT);
        }
        uint16_t other = d->keys[slot];
        if (!other) {
            d->keys[slot] = id;
#ifdef GT_JSON_DYNAMIC
            d->key_count++;
#endif
            return true;
        }
        const json_node_t *o = &d->nodes[other];
        if (o->parent == n->parent && o->key_size == n->key_size &&
            !memcmp(d->strings + o->key, d->strings + n->key, n->key_size)) {
            return fail(d, GT_JSON_DUPLICATE);
        }
    }
}

static bool number(GT_JSON_DOC *d) {
    if (d->input[d->at] == '-') {
        d->at++;
    }
    if (d->at == d->input_size || !digit(d->input[d->at])) {
        return fail(d, GT_JSON_SYNTAX);
    }
    if (d->input[d->at] == '0') {
        d->at++;
    } else {
        while (d->at < d->input_size && digit(d->input[d->at])) {
            d->at++;
        }
    }
    if (d->at < d->input_size && d->input[d->at] == '.') {
        d->at++;
        size_t start = d->at;
        while (d->at < d->input_size && digit(d->input[d->at])) {
            d->at++;
        }
        if (d->at == start) {
            return fail(d, GT_JSON_SYNTAX);
        }
    }
    if (d->at < d->input_size && (d->input[d->at] == 'e' || d->input[d->at] == 'E')) {
        d->at++;
        if (d->at < d->input_size && (d->input[d->at] == '+' || d->input[d->at] == '-')) {
            d->at++;
        }
        size_t start = d->at;
        while (d->at < d->input_size && digit(d->input[d->at])) {
            d->at++;
        }
        if (d->at == start) {
            return fail(d, GT_JSON_SYNTAX);
        }
    }
    return true;
}

static uint16_t
value(GT_JSON_DOC *d, uint16_t parent, uint32_t key, uint32_t key_size, unsigned depth) {
    space(d);
    if (d->at == d->input_size) {
        fail(d, GT_JSON_SYNTAX);
        return 0;
    }
    if (d->used == GT_JSON_NODES) {
        fail(d, GT_JSON_LIMIT);
        return 0;
    }
#ifdef GT_JSON_DYNAMIC
    if (!json_nodes(d, (size_t)d->used + 2)) {
        return 0;
    }
#endif
    uint16_t id = ++d->used;
    json_node_t *n = &d->nodes[id];
    n->parent = parent;
    n->key = key;
    n->key_size = key_size;
    n->start = (uint32_t)d->at;
    if (parent && d->nodes[parent].kind == J_OBJECT && !key_insert(d, id)) {
        return 0;
    }
    uint8_t c = d->input[d->at];
    if (c == '{' || c == '[') {
        if (depth >= GT_JSON_DEPTH) {
            fail(d, GT_JSON_LIMIT);
            return 0;
        }
        n->kind = c == '{' ? J_OBJECT : J_ARRAY;
        uint8_t end = c == '{' ? '}' : ']';
        d->at++;
        space(d);
        if (d->at < d->input_size && d->input[d->at] == end) {
            d->at++;
        } else {
            uint16_t tail = 0;
            for (;;) {
                uint32_t offset = 0, size = 0, points = 0;
                if (n->kind == J_OBJECT) {
                    if (!string(d, &offset, &size, &points)) {
                        return 0;
                    }
                    space(d);
                    if (d->at == d->input_size || d->input[d->at++] != ':') {
                        fail(d, GT_JSON_SYNTAX);
                        return 0;
                    }
                }
                uint16_t child = value(d, id, offset, size, depth + 1);
                if (!child) {
                    return 0;
                }
                n = &d->nodes[id]; /* recursive growth may have moved the node table */
                if (tail) {
                    d->nodes[tail].next = child;
                } else {
                    n->child = child;
                }
                tail = child;
                n->count++;
                space(d);
                if (d->at == d->input_size) {
                    fail(d, GT_JSON_SYNTAX);
                    return 0;
                }
                c = d->input[d->at++];
                if (c == end) {
                    break;
                }
                if (c != ',') {
                    fail(d, GT_JSON_SYNTAX);
                    return 0;
                }
                space(d);
            }
        }
    } else if (c == '"') {
        n->kind = J_STRING;
        if (!string(d, &n->text, &n->size, &n->points)) {
            return 0;
        }
    } else if (c == '-' || digit(c)) {
        n->kind = J_NUMBER;
        if (!number(d)) {
            return 0;
        }
    } else {
        const char *literal = NULL;
        if (c == 't') {
            literal = "true";
            n->kind = J_TRUE;
        } else if (c == 'f') {
            literal = "false";
            n->kind = J_FALSE;
        } else if (c == 'n') {
            literal = "null";
            n->kind = J_NULL;
        }
        if (!literal || d->input_size - d->at < strlen(literal) ||
            memcmp(d->input + d->at, literal, strlen(literal))) {
            fail(d, GT_JSON_SYNTAX);
            return 0;
        }
        d->at += strlen(literal);
    }
    n->bytes = (uint32_t)d->at - n->start;
    return id;
}

/* Exact nonnegative integer value of a JSON number. Scan decimal coefficient
 * and exponent without floating point or locale. Huge zero exponents stay zero;
 * all other out-of-range/fractional results fail before conversion can wrap. */
static bool node_uint(const GT_JSON_DOC *d, uint16_t id, uint64_t *out) {
    const json_node_t *n = &d->nodes[id];
    if (n->kind != J_NUMBER) {
        return false;
    }
    const uint8_t *s = d->input + n->start;
    size_t len = n->bytes, at = s[0] == '-' ? 1 : 0;
    bool negative = at != 0, decimal = false;
    size_t digits = 0, fraction = 0, first = SIZE_MAX, last = 0, coefficient_end;
    for (; at < len && s[at] != 'e' && s[at] != 'E'; at++) {
        if (s[at] == '.') {
            decimal = true;
            continue;
        }
        if (s[at] != '0') {
            if (first == SIZE_MAX) {
                first = digits;
            }
            last = digits;
        }
        digits++;
        if (decimal) {
            fraction++;
        }
    }
    coefficient_end = at;
    int exponent = 0;
    if (at < len) {
        at++;
        bool minus = false;
        if (s[at] == '+' || s[at] == '-') {
            minus = s[at] == '-';
            at++;
        }
        for (; at < len; at++) {
            if (exponent < 100000) {
                exponent = exponent * 10 + s[at] - '0';
            }
        }
        if (minus) {
            exponent = -exponent;
        }
    }
    if (first == SIZE_MAX) {
        *out = 0;
        return true;
    }
    if (negative) {
        return false;
    }
    int64_t shift = (int64_t)exponent - (int64_t)fraction;
    if (shift < 0 && (uint64_t)(-shift) > digits - 1 - last) {
        return false;
    }
    int64_t length = (int64_t)(digits - first) + shift;
    if (length <= 0 || length > 20) {
        return false;
    }
    size_t keep = shift < 0 ? digits - (size_t)(-shift) : digits, index = 0;
    uint64_t result = 0;
    for (at = 0; at < coefficient_end; at++) {
        if (s[at] == '.') {
            continue;
        }
        if (index >= first && index < keep) {
            unsigned v = s[at] - '0';
            if (result > (UINT64_MAX - v) / 10) {
                return false;
            }
            result = result * 10 + v;
        }
        index++;
    }
    for (int64_t i = 0; i < shift; i++) {
        if (result > UINT64_MAX / 10) {
            return false;
        }
        result *= 10;
    }
    *out = result;
    return true;
}
