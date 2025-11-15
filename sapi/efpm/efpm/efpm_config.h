#ifndef EFPM_CONF_H
#define EFPM_CONF_H 1

struct key_value_s;

struct key_value_s {
	struct key_value_s *next;
	char *key;
	char *value;
};

#endif