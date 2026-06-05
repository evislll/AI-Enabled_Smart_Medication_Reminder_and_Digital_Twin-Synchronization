#ifndef AI_PARSER_H_
#define AI_PARSER_H_

#include <stdbool.h>

bool AI_ParseLine(const char* line);
void AI_BootSync(void);
bool AI_BootSyncEthernet(void);

#endif
