CC = clang

all: proxy

proxy: proxy_parse.o proxy_server_with_cache.o
	$(CC) -o proxy proxy_parse.o proxy_server_with_cache.o

proxy_parse.o: proxy_parse.c
	$(CC) -o proxy_parse.o -c proxy_parse.c 

proxy_server_with_cache.o: proxy_server_with_cache.c
	$(CC) -o proxy_server_with_cache.o -c proxy_server_with_cache.c

clean:
	rm -f proxy *.o

.PHONY: all clean


