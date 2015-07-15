#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <unistd.h>

#define BUFFER_SIZE 100

int main(int argc, char **argv){

  char buf[BUFFER_SIZE];
  int fd;
  ssize_t size_read;
  size_t size_to_be_read = BUFFER_SIZE;
  ssize_t size_write;
  size_t size_written;
  size_t size_remaining;
  

  if(argc < 2){
    printf("You are a scrub, this is how you use it: ./My_Cat \"filepath\"\n ");
    exit(EXIT_FAILURE);
  }

  fd = open(argv[1], O_RDONLY);
  if(fd == -1){
    perror("open error: ");
    exit(EXIT_FAILURE);
  }
  
 restart:
  
  while((size_read = read(fd, buf, size_to_be_read)) > 0){

    size_remaining = size_read;
    size_written = 0;
    
    while((size_write = write(STDOUT_FILENO, &buf[size_written],
			      size_remaining)) < size_read){
	     
      if(size_write < 0){
	if(EINTR == errno){
	  continue;
	}
	else {
	  perror("write error: ");
	  exit(EXIT_FAILURE);
	}
      }
      
      else {
	size_written += size_write;
	size_remaining -= size_write;
      }
      
    }
  }
  

  if(size_read < 0){
    if(EINTR == errno){
      goto restart;
    }

    else {
      perror("read error: ");
      exit(EXIT_FAILURE);
    }
  }
  
  close(fd);
  return EXIT_SUCCESS;
}
      
  
