/* ufs.c's __builtin_strcmp on a name read from the disk: gcc expands it inline, clang calls strcmp. */
int strcmp(const char *a, const char *b)
{
	while (*a && *a == *b) {
		a++;
		b++;
	}
	return (unsigned char)*a - (unsigned char)*b;
}
