#include <stdio.h>
#include <unistd.h>

int main()
{
	int ret;
	char buff[1024] = {0};

	if (!getcwd(buff, 1024)) {
		return -1;
	}

	printf("%s\n", buff);

	return 0;
}
