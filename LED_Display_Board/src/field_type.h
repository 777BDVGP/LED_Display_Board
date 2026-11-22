// =============================================================================
// field_type.h — Shared Web-Input Field Type Enum
// =============================================================================
// Used by both settings_store.h and event_store.h so their field tables share
// one type system. Mirrors ARD's WEB_TYPE_* string constants.
// =============================================================================

#ifndef FIELD_TYPE_H
#define FIELD_TYPE_H

typedef enum {
    FIELD_TEXT,
    FIELD_DATE,
    FIELD_TIME,
    FIELD_BOOL,
    FIELD_NUMB,
    FIELD_FLOAT,
    FIELD_DOUBLE,
    FIELD_SELECT,
} field_type_t;

// HTML <input type="..."> string for each field_type_t — for the future web
// page renderer; kept next to the enum so the two can't drift apart.
static inline const char *field_type_html(field_type_t t)
{
    switch (t) {
        case FIELD_TEXT:   return "text";
        case FIELD_DATE:   return "date";
        case FIELD_TIME:   return "time";
        case FIELD_BOOL:   return "checkbox";
        case FIELD_NUMB:   return "number";
        case FIELD_FLOAT:  return "float";
        case FIELD_DOUBLE: return "double";
        case FIELD_SELECT: return "select";
        default:           return "text";
    }
}

#endif // FIELD_TYPE_H
