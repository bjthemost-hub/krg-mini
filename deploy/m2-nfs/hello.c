#include <stdio.h>
#include <unistd.h>

int main(void)
{
	char host[64] = {0};
	gethostname(host, sizeof(host) - 1);
	unsigned long sum = 0;
	for (unsigned long i = 0; i < 1000000UL; i++)
		sum += i;
	printf("hello from %s, sum=%lu\n", host, sum);
	return 0;
}
