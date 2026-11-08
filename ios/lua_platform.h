// Lua's optional shell command is unavailable in an iOS app sandbox.
#define l_system(command) ((command) == nullptr ? 0 : -1)
