/**
 * @file ini_parser.h
 * @brief Minimal INI file parser for OmniRDP configuration
 *
 * Parses [section] + key = value entries. No nesting, no write-back.
 * Dotted keys (e.g. "backend.hostname") are stored verbatim —
 * semantic interpretation is the config layer's responsibility.
 */

#ifndef INI_PARSER_H
#define INI_PARSER_H

#include <stddef.h>

#ifdef __cplusplus
extern "C"
{
#endif

	/** Single key=value entry within a section */
	typedef struct
	{
		char* section; /**< Section name (empty string for global keys) */
		char* key;     /**< Key name (may contain dots, stored verbatim) */
		char* value;   /**< Value string (trimmed of leading/trailing whitespace) */
	} IniEntry;

	/** Parsed INI file */
	typedef struct
	{
		IniEntry* entries; /**< Dynamic array of entries */
		size_t count;      /**< Number of entries */
		size_t capacity;   /**< Allocated capacity */
	} IniFile;

	/**
	 * @brief Parse an INI file from disk
	 * @param filename Path to the INI file
	 * @return Parsed IniFile, or NULL on error (use ini_free to clean up)
	 *
	 * Supports:
	 * - [section] headers
	 * - key = value pairs (whitespace trimmed)
	 * - Comments starting with ; or #
	 * - Blank lines
	 * - Keys without a section (stored with section = "")
	 * - Duplicate keys: last value wins
	 */
	IniFile* ini_parse(const char* filename);

	/**
	 * @brief Look up a value by section and key
	 * @param ini Parsed INI file
	 * @param section Section name (use "" for global keys)
	 * @param key Key name (exact match, including dots)
	 * @param default_val Value to return if key not found
	 * @return Pointer to value string, or default_val if not found.
	 *         The returned pointer is valid until ini_free() is called.
	 */
	const char* ini_get(const IniFile* ini, const char* section, const char* key,
	                    const char* default_val);

	/**
	 * @brief Check if a section exists
	 * @param ini Parsed INI file
	 * @param section Section name to check
	 * @return 1 if section exists, 0 otherwise
	 */
	int ini_has_section(const IniFile* ini, const char* section);

	/**
	 * @brief Get an integer value from the INI file
	 * @param ini Parsed INI file
	 * @param section Section name
	 * @param key Key name
	 * @param default_val Default value if key not found or not a valid integer
	 * @return Parsed integer value
	 */
	int ini_get_int(const IniFile* ini, const char* section, const char* key, int default_val);

	/**
	 * @brief Get an unsigned integer value from the INI file
	 * @param ini Parsed INI file
	 * @param section Section name
	 * @param key Key name
	 * @param default_val Default value if key not found or not a valid integer
	 * @return Parsed unsigned integer value
	 */
	unsigned int ini_get_uint(const IniFile* ini, const char* section, const char* key,
	                          unsigned int default_val);

	/**
	 * @brief Get a boolean value from the INI file
	 * @param ini Parsed INI file
	 * @param section Section name
	 * @param key Key name
	 * @param default_val Default value if key not found
	 * @return 1 if value is "true", "1", "yes", or "on"; 0 otherwise
	 */
	int ini_get_bool(const IniFile* ini, const char* section, const char* key, int default_val);

	/**
	 * @brief Free a parsed INI file
	 * @param ini Parsed INI file (may be NULL)
	 */
	void ini_free(IniFile* ini);

#ifdef __cplusplus
}
#endif

#endif /* INI_PARSER_H */