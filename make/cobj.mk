%.o: $(S)/%.c
	$(CC) $(CFLAGS) $(EXTRA_CFLAGS) $(REENT_CFLAGS) -c -o $@ $<

# used by dlm/libdlm
%_lt.o: $(S)/%.c
	$(CC) $(CFLAGS) $(EXTRA_CFLAGS) -c -o $@ $<
