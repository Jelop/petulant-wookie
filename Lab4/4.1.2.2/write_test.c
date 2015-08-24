#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <fcntl.h>
#include <string.h>

int main(int argc, char *argv[])
{
	int length = 20, position = 0, fd, rc;
	char *message, *nodename = "/dev/mycdrv";

	/* set up the message */
	message = malloc(length);
	memset(message, 'x', length);
	message[length - 1] = '\0';	/* make sure it is null terminated */

	/* open the device node */

	fd = open(nodename, O_RDWR);
	printf(" I opened the device node, file descriptor = %d\n", fd);

	/* seek to position */

	rc = lseek(fd, position, SEEK_SET);
	printf("return code from lseek = %d\n", rc);

	/* write to the device node twice */

	rc = write(fd, message, length);
	printf("return code from write = %d\n", rc);
	rc = write(fd, message, length);
	printf("return code from write = %d\n", rc);

	/* reset the message to null */

	memset(message, 0, length);


	close(fd);
	exit(0);

}
