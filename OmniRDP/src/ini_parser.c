/**
 * @file ini_parser.c
 * @brief Minimal INI file parser for OmniRDP configuration
 *
 * Parses [section] + key = value entries. No nesting, no write-back.
 * ~150 lines of actual parsing logic, zero external dependencies.
 */

#include "ini_parser.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Internal helpers ──────────────────────────────────────────── */

static IniEntry*
ini_find(const IniFile* ini, const char* section, const char* key)
{
	for (size_t i = 0; i < ini->count; i++)
	{
		if (strcmp(ini->entries[i].section, section) == 0 && strcmp(ini->entries[i].key, key) == 0)
		{
			return &ini->entries[i];
		}
	}
	return NULL;
}

static int
ini_push(IniFile* ini, char* section, char* key, char* value)
{
	/* Duplicate key in same section: last value wins */
	IniEntry* existing = ini_find(ini, section, key);
	if (existing)
	{
		free(existing->value);
		existing->value = value;
		free(section);
		free(key);
		return 0;
	}

	if (ini->count >= ini->capacity)
	{
		size_t new_cap = ini->capacity ? ini->capacity * 2 : 64;
		IniEntry* new_entries = (IniEntry*)realloc(ini->entries, new_cap * sizeof(IniEntry));
		if (!new_entries)
		{
			free(section);
			free(key);
			free(value);
			return -1;
		}
		ini->entries = new_entries;
		ini->capacity = new_cap;
	}

	ini->entries[ini->count].section = section;
	ini->entries[ini->count].key = key;
	ini->entries[ini->count].value = value;
	ini->count++;
	return 0;
}

static char*
trim(char* s)
{
	while (*s && isspace((unsigned char)*s))
		s++;
	if (*s == '\0')
		return s;
	char* end = s + strlen(s) - 1;
	while (end > s && isspace((unsigned char)*end))
		*end-- = '\0';
	return s;
}

/* ── Public API ─────────────────────────────────────────────────── */

IniFile*
ini_parse(const char* filename)
{
	FILE* fp = fopen(filename, "r");
	if (!fp)
		return NULL;

	IniFile* ini = (IniFile*)calloc(1, sizeof(IniFile));
	if (!ini)
	{
		fclose(fp);
		return NULL;
	}

	char line[4096];
	char current_section[256] = ""; /* Global section */

	while (fgets(line, sizeof(line), fp))
	{
		/* Strip trailing newline */
		size_t len = strlen(line);
		while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
			line[--len] = '\0';

		/* Skip leading whitespace */
		char* p = line;
		while (*p && isspace((unsigned char)*p))
			p++;

		/* Skip empty lines and comments */
		if (*p == '\0' || *p == ';' || *p == '#')
			continue;

		/* Section header: [name] */
		if (*p == '[')
		{
			char* end = strchr(p, ']');
			if (end)
			{
				*end = '\0';
				char* name = trim(p + 1);
				strncpy(current_section, name, sizeof(current_section) - 1);
				current_section[sizeof(current_section) - 1] = '\0';
			}
			continue;
		}

		/* Key = value */
		char* eq = strchr(p, '=');
		if (!eq)
			continue;

		*eq = '\0';
		char* key = trim(p);
		char* value = trim(eq + 1);

		if (key[0] == '\0')
			continue;

		char* sec_copy = _strdup(current_section);
		char* key_copy = _strdup(key);
		char* val_copy = _strdup(value);

		if (!sec_copy || !key_copy || !val_copy)
		{
			free(sec_copy);
			free(key_copy);
			free(val_copy);
			ini_free(ini);
			fclose(fp);
			return NULL;
		}

		if (ini_push(ini, sec_copy, key_copy, val_copy) != 0)
		{
			/* ini_push frees section/key/value on failure */
			ini_free(ini);
			fclose(fp);
			return NULL;
		}
	}

	fclose(fp);
	return ini;
}

const char*
ini_get(const IniFile* ini, const char* section, const char* key, const char* default_val)
{
	if (!ini)
		return default_val;
	IniEntry* entry = ini_find(ini, section, key);
	return entry ? entry->value : default_val;
}

int
ini_has_section(const IniFile* ini, const char* section)
{
	if (!ini)
		return 0;
	for (size_t i = 0; i < ini->count; i++)
	{
		if (strcmp(ini->entries[i].section, section) == 0)
			return 1;
	}
	return 0;
}

int
ini_get_int(const IniFile* ini, const char* section, const char* key, int default_val)
{
	const char* val = ini_get(ini, section, key, NULL);
	if (!val)
		return default_val;
	char* end;
	long result = strtol(val, &end, 10);
	if (end == val || *end != '\0')
		return default_val;
	return (int)result;
}

unsigned int
ini_get_uint(const IniFile* ini, const char* section, const char* key, unsigned int default_val)
{
	const char* val = ini_get(ini, section, key, NULL);
	if (!val)
		return default_val;
	char* end;
	unsigned long result = strtoul(val, &end, 10);
	if (end == val || *end != '\0')
		return default_val;
	return (unsigned int)result;
}

int
ini_get_bool(const IniFile* ini, const char* section, const char* key, int default_val)
{
	const char* val = ini_get(ini, section, key, NULL);
	if (!val)
		return default_val;
	/* Match true/1/yes/on (case-insensitive) */
	if (_stricmp(val, "true") == 0 || strcmp(val, "1") == 0 || _stricmp(val, "yes") == 0 ||
	    _stricmp(val, "on") == 0)
	{
		return 1;
	}
	if (_stricmp(val, "false") == 0 || strcmp(val, "0") == 0 || _stricmp(val, "no") == 0 ||
	    _stricmp(val, "off") == 0)
	{
		return 0;
	}
	return default_val;
}

void
ini_free(IniFile* ini)
{
	if (!ini)
		return;
	for (size_t i = 0; i < ini->count; i++)
	{
		free(ini->entries[i].section);
		free(ini->entries[i].key);
		free(ini->entries[i].value);
	}
	free(ini->entries);
	free(ini);
}