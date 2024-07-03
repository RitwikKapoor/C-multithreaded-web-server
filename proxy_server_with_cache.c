#include "proxy_parse.h"
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

typedef struct cache_element cache_element;

#define SEMAPHORE_NAME "/my_semaphore"
#define MAX_BYTES 4096 // max allowed size of request/response
#define MAX_CLIENTS 10 // max number of client requests served at a time
#define MAX_SIZE 200 * (1 << 20)   // size of the cache
#define MAX_ELEMENT_SIZE (1 << 20) // max size of an element in cache

struct cache_element {
  char *data;
  int len;
  char *url;
  time_t lru_time_track;
  cache_element *next;
};

int port_number = 8080;
int proxy_socketId;
sem_t *semaphore;
pthread_t tid[MAX_CLIENTS];
pthread_mutex_t lock;

cache_element *head;
int cache_size;

cache_element *find(char *url);
int add_cache_element(char *data, int size, char *url);
void remove_cache_element();

int sendErrorMessage(int socket, int status_code) {
  char str[1024];
  char currentTime[50];
  time_t now = time(0);

  struct tm data = *gmtime(&now);
  strftime(currentTime, sizeof(currentTime), "%a, %d %b %Y %H:%M:%S %Z", &data);

  switch (status_code) {
  case 400:
    snprintf(str, sizeof(str),
             "HTTP/1.1 400 Bad Request\r\nContent-Length: 95\r\nConnection: "
             "keep-alive\r\nContent-Type: text/html\r\nDate: %s\r\nServer: "
             "VaibhavN/14785\r\n\r\n<HTML><HEAD><TITLE>400 Bad "
             "Request</TITLE></HEAD>\n<BODY><H1>400 Bad "
             "Rqeuest</H1>\n</BODY></HTML>",
             currentTime);
    printf("400 Bad Request\n");
    send(socket, str, strlen(str), 0);
    break;

  case 403:
    snprintf(str, sizeof(str),
             "HTTP/1.1 403 Forbidden\r\nContent-Length: 112\r\nContent-Type: "
             "text/html\r\nConnection: keep-alive\r\nDate: %s\r\nServer: "
             "VaibhavN/14785\r\n\r\n<HTML><HEAD><TITLE>403 "
             "Forbidden</TITLE></HEAD>\n<BODY><H1>403 "
             "Forbidden</H1><br>Permission Denied\n</BODY></HTML>",
             currentTime);
    printf("403 Forbidden\n");
    send(socket, str, strlen(str), 0);
    break;

  case 404:
    snprintf(
        str, sizeof(str),
        "HTTP/1.1 404 Not Found\r\nContent-Length: 91\r\nContent-Type: "
        "text/html\r\nConnection: keep-alive\r\nDate: %s\r\nServer: "
        "VaibhavN/14785\r\n\r\n<HTML><HEAD><TITLE>404 Not "
        "Found</TITLE></HEAD>\n<BODY><H1>404 Not Found</H1>\n</BODY></HTML>",
        currentTime);
    printf("404 Not Found\n");
    send(socket, str, strlen(str), 0);
    break;

  case 500:
    snprintf(
        str, sizeof(str),
        "HTTP/1.1 500 Internal Server Error\r\nContent-Length: "
        "115\r\nConnection: keep-alive\r\nContent-Type: text/html\r\nDate: "
        "%s\r\nServer: VaibhavN/14785\r\n\r\n<HTML><HEAD><TITLE>500 Internal "
        "Server Error</TITLE></HEAD>\n<BODY><H1>500 Internal Server "
        "Error</H1>\n</BODY></HTML>",
        currentTime);

    send(socket, str, strlen(str), 0);
    break;

  case 501:
    snprintf(
        str, sizeof(str),
        "HTTP/1.1 501 Not Implemented\r\nContent-Length: 103\r\nConnection: "
        "keep-alive\r\nContent-Type: text/html\r\nDate: %s\r\nServer: "
        "VaibhavN/14785\r\n\r\n<HTML><HEAD><TITLE>404 Not "
        "Implemented</TITLE></HEAD>\n<BODY><H1>501 Not "
        "Implemented</H1>\n</BODY></HTML>",
        currentTime);
    printf("501 Not Implemented\n");
    send(socket, str, strlen(str), 0);
    break;

  case 505:
    snprintf(
        str, sizeof(str),
        "HTTP/1.1 505 HTTP Version Not Supported\r\nContent-Length: "
        "125\r\nConnection: keep-alive\r\nContent-Type: text/html\r\nDate: "
        "%s\r\nServer: VaibhavN/14785\r\n\r\n<HTML><HEAD><TITLE>505 HTTP "
        "Version Not Supported</TITLE></HEAD>\n<BODY><H1>505 HTTP Version Not "
        "Supported</H1>\n</BODY></HTML>",
        currentTime);
    printf("505 HTTP Version Not Supported\n");
    send(socket, str, strlen(str), 0);
    break;

  default:
    return -1;
  }
  return 1;
}

int checkHTTPversion(char *msg) {
  int version = -1;

  if (strncmp(msg, "HTTP/1.1", 8) == 0) {
    version = 1;
  } else if (strncmp(msg, "HTTP/1.0", 8) == 0) {
    version = 1;
  }

  return version;
}

int connectRemoteServer(char *host_addr, int port_num) {
  int remoteSocket = socket(AF_INET, SOCK_STREAM, 0);

  if (remoteSocket < 0) {
    printf("Error in Creating Socket.\n");
    return -1;
  }

  // Get host by the name or ip address provided
  struct hostent *host = gethostbyname(host_addr);
  if (host == NULL) {
    perror("No such host exists.\n");
    return -1;
  }

  struct sockaddr_in server_addr;

  memset((char *)&server_addr, 0, sizeof(server_addr));
  server_addr.sin_family = AF_INET;
  server_addr.sin_port = htons(port_num);

  memcpy(&server_addr.sin_addr.s_addr, host->h_addr, host->h_length);

  // Connect to Remote server
  if (connect(remoteSocket, (struct sockaddr *)&server_addr,
              (socklen_t)sizeof(server_addr)) < 0) {
    fprintf(stderr, "Error in connecting !\n");
    return -1;
  }

  return remoteSocket;
}

int handle_request(int clientSocket, struct ParsedRequest *request,
                   char *tempReq) {
  char *buf = (char *)malloc(sizeof(char) * MAX_BYTES);
  strcpy(buf, "GET ");
  strcat(buf, request->path);
  strcat(buf, " ");
  strcat(buf, request->version);
  strcat(buf, "\r\n");

  size_t len = strlen(buf);

  if (ParsedHeader_set(request, "Connection", "close") < 0) {
    printf("set header key does not work\n");
  }

  if (ParsedHeader_get(request, "Host") == NULL) {
    if (ParsedHeader_set(request, "Host", request->host) < 0) {
      printf("Set \"Host\" header key not working\n");
    }
  }

  if (ParsedRequest_unparse_headers(request, buf + len,
                                    (size_t)MAX_BYTES - len) < 0) {
    printf("unparse failed\n");
  }

  int server_port = 80; // Default Remote Server Port
  if (request->port != NULL)
    server_port = atoi(request->port);

  int remoteSocketID = connectRemoteServer(request->host, server_port);

  if (remoteSocketID < 0) {
    return -1;
  }

  int bytes_send = send(remoteSocketID, buf, strlen(buf), 0);

  memset(buf, 0, MAX_BYTES);

  int bytes_received_from_remote = recv(remoteSocketID, buf, MAX_BYTES - 1, 0);
  char *temp_buffer = (char *)malloc(sizeof(char) * MAX_BYTES);
  int temp_buffer_size = MAX_BYTES;
  int temp_buffer_index = 0;

  while (bytes_received_from_remote > 0) {
    bytes_received_from_remote = send(clientSocket, buf, bytes_send, 0);

    for (int i = 0; i < bytes_received_from_remote / sizeof(char); i++) {
      temp_buffer[temp_buffer_index] = buf[i];
      temp_buffer_index++;
    }
    temp_buffer_size += MAX_BYTES;
    temp_buffer = (char *)realloc(temp_buffer, temp_buffer_size);

    if (bytes_received_from_remote < 0) {
      perror("Error in sending data to client socket.\n");
      break;
    }
    memset(buf, 0, MAX_BYTES);

    bytes_received_from_remote = recv(remoteSocketID, buf, MAX_BYTES, 0);
  }

  // temp_buffer[temp_buffer_index] = '\0';
  free(buf);
  add_cache_element(temp_buffer, strlen(temp_buffer), tempReq);
  printf("Done\n");
  free(temp_buffer);

  close(remoteSocketID);
  return 0;
}

void *thread_fn(void *arg) {
  int socketId = *(int *)arg;
  sem_t *semaphore = sem_open(SEMAPHORE_NAME, 0);
  sem_wait(semaphore);

  char *buffer = (char *)calloc(MAX_BYTES, sizeof(char));
  memset(buffer, 0, MAX_BYTES);

  int bytes_received_from_client, len;
  bytes_received_from_client = recv(socketId, buffer, MAX_BYTES, 0);

  while (bytes_received_from_client > 0) {
    len = strlen(buffer);

    if (strstr(buffer, "\r\n\r\n") == NULL) {
      bytes_received_from_client =
          recv(socketId, buffer + len, MAX_BYTES - len, 0);
    } else {
      break;
    }
  }

  char *tempReq = (char *)malloc(strlen(buffer) * sizeof(char));
  for (int i = 0; i < strlen(buffer); i++) {
    tempReq[i] = buffer[i];
  }

  cache_element *temp = find(tempReq);
  if (temp != NULL) {
    int size = temp->len / sizeof(char);
    int pos = 0;
    char response[MAX_BYTES];
    while (pos < size) {
      bzero(response, MAX_BYTES);
      for (int i = 0; i < MAX_BYTES; i++) {
        response[i] = temp->data[pos];
        pos++;
      }
      send(socketId, response, MAX_BYTES, 0);
    }
    printf("Data retrived from the Cache\n\n");
    printf("%s\n\n", response);
  } else if (bytes_received_from_client > 0) {
    len = strlen(buffer);
    struct ParsedRequest *request = ParsedRequest_create();
    if (ParsedRequest_parse(request, buffer, len) < 0) {
      printf("Parsing failed\n");
    } else {
      memset(buffer, 0, MAX_BYTES);
      if (!strcmp(request->method, "GET")) {
        if (request->host && request->path &&
            (checkHTTPversion(request->version) == 1)) {
          bytes_received_from_client =
              handle_request(socketId, request, tempReq);
          if (bytes_received_from_client == -1) {
            sendErrorMessage(socketId, 500);
          }
        } else {
          sendErrorMessage(socketId, 500);
        }
      } else {
        printf("This code doesn't support any method other than GET\n");
      }
    }
    ParsedRequest_destroy(request);
  } else if (bytes_received_from_client < 0) {
    perror("Error in receiving from client.\n");
  } else if (bytes_received_from_client == 0) {
    printf("Client disconnected!\n");
  }

  shutdown(socketId, SHUT_RDWR);
  close(socketId);
  free(buffer);

  sem_post(semaphore);
  sem_close(semaphore);
  free(tempReq);
  return NULL;
}

cache_element *find(char *url) {
  pthread_mutex_lock(&lock);
  printf("Searching for URL: %s\n", url);

  cache_element *current = head;
  while (current != NULL) {
    if (strcmp(current->url, url) == 0) {
      // URL found, update LRU time and return the element
      current->lru_time_track = time(NULL);
      pthread_mutex_unlock(&lock);
      printf("URL found in cache: %s\n", url);
      return current;
    }
    current = current->next;
  }

  pthread_mutex_unlock(&lock);
  printf("URL not found in cache: %s\n", url);
  return NULL;
}

int add_cache_element(char *data, int size, char *url) {
  int temp_lock_val = pthread_mutex_lock(&lock);
  printf("Add Cache Lock Acquired %d\n", temp_lock_val);

  int element_size = size + 1 + strlen(url) + sizeof(cache_element);
  printf(
      "size of element and cache and max_element size are %d and %d and %d\n",
      element_size, cache_size, MAX_ELEMENT_SIZE);

  if (element_size > MAX_ELEMENT_SIZE) {
    temp_lock_val = pthread_mutex_unlock(&lock);
    printf("Add Cache Lock Unlocked as size exceeded %d\n", temp_lock_val);
    return 0; // Element size exceeds maximum allowed size
  } else {
    while (cache_size + element_size > MAX_SIZE) {
      // Remove elements from cache until enough space is available
      printf("BEFORE: size of element and cache and max_cache size are %d and "
             "%d and %d\n",
             element_size, cache_size, MAX_SIZE);
      remove_cache_element();
      printf("AFTER: size of element and cache and max_cache size are %d and "
             "%d and %d\n",
             element_size, cache_size, MAX_SIZE);
    }

    cache_element *element = (cache_element *)malloc(sizeof(cache_element));
    element->data = (char *)malloc(size + 1);
    strcpy(element->data, data);
    element->url = (char *)malloc(strlen(url) + 1);
    strcpy(element->url, url);
    element->len = size;
    element->lru_time_track = time(NULL);
    element->next = head;
    head = element;
    cache_size += element_size;

    temp_lock_val = pthread_mutex_unlock(&lock);
    printf("Add Cache Lock Unlocked %d\n", temp_lock_val);
    return 1;
  }
}

void remove_cache_element() {
  printf("Removing least recently used element\n");

  if (head == NULL) { // Cache is empty
    return;
  }

  cache_element *prev = NULL;
  cache_element *current = head;
  cache_element *lru_prev = NULL;
  cache_element *lru = head;

  // Find the least recently used (LRU) element
  while (current != NULL) {
    if (current->lru_time_track < lru->lru_time_track) {
      lru = current;
      lru_prev = prev;
    }
    prev = current;
    current = current->next;
  }

  // Remove the LRU element from the linked list
  if (lru_prev == NULL) {
    head = lru->next; // LRU is the head element
  } else {
    lru_prev->next = lru->next;
  }

  cache_size -= (lru->len + 1 + sizeof(cache_element) + strlen(lru->url));

  free(lru->data);
  free(lru->url);
  free(lru);
}

int main(int argc, char *argv[]) {
  int client_socketId, client_len;
  struct sockaddr_in server_addr, client_addr;
  sem_t *semaphore = sem_open(SEMAPHORE_NAME, O_CREAT, 0644, MAX_CLIENTS);
  pthread_mutex_init(&lock, NULL);

  if (argc == 2) {
    port_number = atoi(argv[1]);
  } else {
    printf("Too few arguments\n");
    exit(1);
  }

  printf("Setting Proxy Server Port : %d\n", port_number);
  proxy_socketId = socket(AF_INET, SOCK_STREAM, 0);

  if (proxy_socketId < 0) {
    perror("Failed to create a socket\n");
    exit(1);
  }

  int reuse = 1;
  if (setsockopt(proxy_socketId, SOL_SOCKET, SO_REUSEADDR, (const char *)&reuse,
                 sizeof(reuse)) < 0) {
    perror("setsockopt(SO_REUSEADDR) failed");
  }

  memset((char *)&server_addr, 0, sizeof(server_addr));

  server_addr.sin_family = AF_INET;
  server_addr.sin_port = htons(port_number);
  server_addr.sin_addr.s_addr = INADDR_ANY;

  if (bind(proxy_socketId, (struct sockaddr *)&server_addr,
           sizeof(server_addr)) < 0) {
    perror("Could not bind");
    exit(1);
  }

  printf("Binding on port: %d\n", port_number);

  int listen_status = listen(proxy_socketId, MAX_CLIENTS);
  if (listen_status < 0) {
    perror("Error while Listening !\n");
    exit(1);
  }

  int i = 0;
  int Connected_socketId[MAX_CLIENTS];

  while (1) {
    memset((char *)&client_addr, 0, sizeof(client_addr));
    client_len = sizeof(client_addr);
    client_socketId = accept(proxy_socketId, (struct sockaddr *)&client_addr,
                             (socklen_t *)&client_len);
    if (client_socketId < 0) {
      fprintf(stderr, "Error in Accepting connection !\n");
      exit(1);
    } else {
      Connected_socketId[i] = client_socketId;
    }

    // Getting IP address and port number of client
    struct sockaddr_in *client_pt = (struct sockaddr_in *)&client_addr;
    struct in_addr ip_addr = client_pt->sin_addr;
    char str[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &ip_addr, str, INET_ADDRSTRLEN);
    printf("Client is connected with port number: %d and ip address: %s \n",
           ntohs(client_addr.sin_port), str);
    pthread_create(&tid[i], NULL, thread_fn, (void *)&Connected_socketId[i]);
    i++;
  }
  close(proxy_socketId);
  sem_close(semaphore);
  sem_unlink(SEMAPHORE_NAME);
  return 0;
}
