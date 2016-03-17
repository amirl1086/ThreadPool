
#include <stdio.h>
#include <stdlib.h>
#include <netinet/in.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/types.h>
#include <time.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include "threadpool.h"

//macros
#define FAILURE -1
#define SUCCESS 0
#define KILOBYTE 1024
#define SYSTEM_ERROR 0
#define LOCAL_ERROR 1
#define RFC1123FMT "%a, %d %b %Y %H:%M:%S GMT"
//response codes
#define WRITE_ERROR 0
#define OK 200
#define OK_FILE 201
#define OK_FOLDER 202
#define FOUND 302
#define BAD_REQUEST 400
#define FORBIDDEN 403
#define NOT_FOUND 404
#define INTERNAL_ERROR 500
#define NOT_SUPPORTED 501
#define DEFAULT_PROTOCOL "HTTP/1.0"

//private functions - further information below
int dispatch_function(void*);

//dispatch function is calling:
/* 1. */int read_from_socket(int, char*);
/* 2. */int parse_header(char*, char*, char*, int*);
/* 3. */int parse_path(char*, int*);

//and then by the code value will be called one of the following:
void send_error_response(int, char*, char*, char*, int);
int send_file_response(int, char*, char*, char*, int*);
int send_folder_response(int, char*, char*, char*, int*);

//these 3 functions mantioned above will use the following:
char *code_to_string(int);
char *get_mime_type(char*);
int write_to_socket(int, char*, int);

//private functions for setting up the server
int parse_args(char*[], int*, int*, int*);
int digits_only(char*);
int set_up_server(struct sockaddr_in*, int*, int);

//the main function (the main thread) - will set up the server
int main(int argc, char *argv[])
{
	//variables
	int listen_socket, new_socket ,*client_socket, counter = 0;
	struct sockaddr_in my_server, client_addr;
	socklen_t socklen = sizeof(struct sockaddr_in);

	//check the number of arguments from the shell
	if (argc != 4)
	{
		printf("Usage: server <port> <pool-size> <max-requests-number>\n");
		exit(EXIT_FAILURE);
	}
	
	//parse the arguments requested
	int port, pool_size, max_requests;
	if (parse_args(argv, &port, &pool_size, &max_requests) < 0)
	{
		printf("Illegal input\n");
		exit(EXIT_FAILURE);
	}
	
	//create a pool of threads
	threadpool *pool = create_threadpool(pool_size);
	if (!pool) //caused by memory, mutex, condition variables or threads initialtion failure
	{
		perror("pool");
		exit(EXIT_FAILURE);
	}
	
	//set up the TCP server
	if (set_up_server(&my_server, &listen_socket, port) < 0)
	{
		destroy_threadpool(pool);
		exit(EXIT_FAILURE);
	}
	
	//wait and accept incoming requests
	while(counter < max_requests)
	{
		bzero((char*)&client_addr, sizeof(struct sockaddr_in)); //init 0 for each client
		// accept will initialize a new socket to the client's request
		new_socket = accept(listen_socket, (struct sockaddr*)&client_addr, &socklen);
		//the server is running, we don't want over one unsuccessful socket to be terminated
		if (new_socket < 0) //don't shutdown over one unsuccessful socket
			perror("opening new socket\n");
		else
		{	//let the threads know that there is a new request
			client_socket = (int*)calloc(1, sizeof(int));
			if (!client_socket)
				perror("allocating memory");
			else
			{
				*client_socket = new_socket;
				dispatch(pool, dispatch_function, (void*)client_socket);
				counter++;
			}
		}
	}
	//shutting down
	close(listen_socket);
	destroy_threadpool(pool);
	return SUCCESS; 
}

//the function of the threads
int dispatch_function(void *arg)
{
	//variables
	int socket_fd = *((int*)(arg)), //casting before going to work
		code = 0; //the code, will function like errno
	free(arg);
	char msg_received[4 * KILOBYTE] = { 0 },
		path[PATH_MAX] = { 0 }, protocol[9] = { DEFAULT_PROTOCOL },
		tb_now[32] = { 0 },
		*header_check = NULL;
	time_t now;
	
	/* all the functions below (except "send_error_response(..)")
	will return -1 (FAILURE) incase one of the macro errors occurred.
	the variable "code" will be set appropriately for further use */
	
	//set up the current time
	now = time(NULL);
	strftime(tb_now, sizeof(tb_now), RFC1123FMT, gmtime(&now));
	
	//read the request from the socket
	if (read_from_socket(socket_fd, msg_received) < 0)
	{
		perror("read");
		close(socket_fd);
		return FAILURE;
	}
	
	//if received an empty message
	if (!strlen(msg_received))
	{
		close(socket_fd);
		return FAILURE;
	}
	
	//http request will be only the first header
	header_check = strstr(msg_received, "\r\n");
	if (!header_check) // if = NULL, "\r\n" wasn't found, illegal http request
	{
		send_error_response(socket_fd, NULL, protocol, tb_now, BAD_REQUEST);
		close(socket_fd);
		return FAILURE;
	}
	header_check[0] = '\0'; //cut the request to focus on the first header

	//parsing the request by the client
	if (parse_header(msg_received, path, protocol, &code) < 0)
	{
		send_error_response(socket_fd, path, protocol, tb_now, code);
		close(socket_fd);
		return FAILURE;
	}
	
	//will parse the given path
	if (parse_path(path, &code) < 0)
	{
		send_error_response(socket_fd, path, protocol, tb_now, code);
		close(socket_fd);
		return FAILURE;
	}
	
	//send the response according to the code
	if (code == OK_FILE)
	{	//send the file information
		if(send_file_response(socket_fd, path, protocol, tb_now, &code) < 0)
		{
			//if a perror accoured we don't want 
			if (code != WRITE_ERROR)
				send_error_response(socket_fd, path, protocol, tb_now, code);
			close(socket_fd);
			return FAILURE;
		}
	}
	else if (code == OK_FOLDER)
	{	//send the entire folder content information
		if(send_folder_response(socket_fd, path, protocol, tb_now, &code) < 0)
		{
			if (code != WRITE_ERROR)
				send_error_response(socket_fd, path, protocol, tb_now, code);
			close(socket_fd);
			return FAILURE;
		}
	}
		
	close(socket_fd);
	return SUCCESS;
}

//this function will read from a socket
int read_from_socket(int socket_fd, char *msg_received)
{
	int bytes_read;
	char temp_data[KILOBYTE * 2] = { 0 };
	while (1)
	{
		bytes_read = read(socket_fd, temp_data, sizeof(temp_data));
		if (bytes_read < 0)
			return FAILURE;
		else if (bytes_read > 0)
		{
			sprintf(msg_received + strlen(msg_received), "%s", temp_data);
			if (strstr(temp_data, "\r\n")) //found "\r\n"
				break;
			bzero(temp_data, sizeof(temp_data));
		}
		else
			break;
	}
	return SUCCESS;
}

//prase and validate the first header
int parse_header(char *http_request, char *path, char *protocol, int *code)
{
	//finding the protocol, check that at least one of them exists
	char *temp_protocol_0 = strstr(http_request, "HTTP/1.0");
	char *temp_protocol_1 = strstr(http_request, "HTTP/1.1");
	
	//if both = NULL, that means bad input by client (no protocol)
	//if both != NULL, also bad input by client (expecting only one version)
	if ((!temp_protocol_0 && !temp_protocol_1) || (temp_protocol_0 && temp_protocol_1))
	{
		*code = BAD_REQUEST;
		return FAILURE;
	}
	
	//check that there is a "/r/n" string right after the protocol
	//this check will verify that the protocol is the last argument
	if (temp_protocol_0) //first version (1.0)
	{
		strcpy(protocol, "HTTP/1.0");
		if (temp_protocol_0[8] != '\0') //not the last argument
		{
			*code = BAD_REQUEST;
			return FAILURE;
		}
			
	}
	else if (temp_protocol_1) //second version (1.1)
	{
		strcpy(protocol, "HTTP/1.1");
		if (temp_protocol_1[8] != '\0') //not the last argument
		{
			*code = BAD_REQUEST;
			return FAILURE;
		}
	}

	//checking the method, get the begining of the request with the first space
	if (strncmp(http_request, "GET ", 4))
	{
		*code = NOT_SUPPORTED;
		return FAILURE;
	}
	http_request += 4; //skipping through the method
	
	//finding the path - everything between the method and the path
	//n - the length of the path, the number ofthe characters between the path and the protocol
	int n = strlen(http_request) - (strlen(protocol) + 1);
	if (http_request[n] != ' ')
	{
		*code = BAD_REQUEST;
		return FAILURE;
	}
	//copy the path into the allocated space
	strncpy(path, http_request, n);
	
	char path_buff[PATH_MAX] = { 0 };
	//if the request arrived from a webpage, look for %20
	if (strstr(path, "%20")) //url spaces in the path, needs to be removed
	{
		int i, j;
		for (i = 0, j = 0; i < n; i++, j++)
		{	//compare the addresses
			if (!strncmp(&(path[i]), "%20", 3))
			{
				i += 2;
				path_buff[j] = ' ';
			}
			else
				path_buff[j] = path[i];
		}
		bzero(path, strlen(path));
		strcpy(path, path_buff);
		bzero(path_buff, strlen(path_buff));
	}
	
	//setting up the current folder as the root directory
	sprintf(path_buff, ".%s", path);
	sprintf(path, "%s", path_buff);
	
	return SUCCESS;
}

//validate the path in the given request
int parse_path(char *path, int *code)
{
	//variables
	struct stat file_info = { 0 };
	struct dirent *file_entity = NULL;
	DIR *folder = NULL;
	int path_length = strlen(path);
	
	/* lstat() - execute permission is required on all of the directories in
	path that lead to the file (but not the file/folder in the end of the path). */
	if (lstat(path, &file_info) < 0)
	{ //if lstat() < 0, errno will be set appropriately
		if (errno == ENOENT) //doesn't exist or empty
			*code = NOT_FOUND;
		//execute permission is denied for one of the directories in the path
		else if (errno == EACCES) 
			*code = FORBIDDEN;
		else //other errors will be treated as syetem errors
			*code = INTERNAL_ERROR;
		return FAILURE;
	}
	//if the path is an existing folder
	if (S_ISDIR(file_info.st_mode))
 	{
 		//check the path for "/" at the end
		if(path[path_length - 1] != '/') //doesn't end with "/"
		{
			*code = FOUND;
			return FAILURE;
		}
		else //ends with "/" look for index.html
		{	//opendir() - opens a directory stream corresponding to the path
			folder = opendir(path);
			if (!folder)
			{	//execute permission is denied for one of the directories in the path
				if (errno == EACCES) 
					*code = FORBIDDEN;
				else //other errors will be treated as syetem errors
					*code = INTERNAL_ERROR;
				return FAILURE;
			}
			file_entity = readdir(folder);
			/* readdir() function returns a pointer to a dirent structure representing 
			the next directory entry in the directory stream pointed to by folder. */
			while (file_entity) 
			{
				//found a file named "index.html" in the requested folder
				if (!strcmp(file_entity->d_name, "index.html"))
				{
					//appending the file name "index.html" to the path
					strcat(path, "index.html");
					//get the stat of the file pointed to by the new path.
					if (lstat(path, &file_info) < 0)
					{ 	//execute permission is denied for one of the directories in the path
						if (errno == EACCES) 
							*code = FORBIDDEN;
						else //other errors will be treated as syetem errors
							*code = INTERNAL_ERROR;
						return FAILURE;
					}
					*code = OK_FILE;
					closedir(folder);
					return SUCCESS;
				}
				//go to the next file, returns NULL upon end of stream
				file_entity = readdir(folder);
			}
			/* if reached here, that means that the file "index.html" wasn't 
			found, send the entire folder content */
			*code = OK_FOLDER;
			closedir(folder);
		}
	} //if the path is an existing regular file 
	else if (S_ISREG(file_info.st_mode))
	{
		*code = OK_FILE;
	}
	else //the path exists but it is not a dir nor regualr file
	{
		*code = FORBIDDEN;
		return FAILURE;
	}
	return SUCCESS;
}

//send an error response with the proper code
void send_error_response(int socket_fd, char *path, char *protocol, char *tb_now, int code)
{
	//variables
	char html_code[KILOBYTE / 2] = { 0 }, headers[KILOBYTE / 2] = { 0 },
		response[KILOBYTE] = { 0 },
		*string_code = code_to_string(code);
	
	//build the headers and the http code
	sprintf(headers,
		"%s %s\r\nServer: webserver/1.%s\r\nDate: %s\r\n%s%s%sContent-Type: %s\r\n",
		protocol, string_code, protocol[7] == '0'? "0" : "1", tb_now, 
		code == FOUND? "Location: " : "", code == FOUND? (path + 1) : "", //skip the "."
		code == FOUND? "/\r\n" : "", "text/html");
	
	sprintf(html_code,
		"<HTML><HEAD><TITLE>%s</TITLE></HEAD>\r\n<BODY><H4>%s</H4>%s</BODY></HTML>\r\n\r\n",
		string_code, string_code,
		code == FOUND? "Directories must end with a slash." : 
		code == BAD_REQUEST? "Bad Request." :
		code == FORBIDDEN? "Access denied." :
		code == NOT_FOUND? "File not found." :
		code == INTERNAL_ERROR? "Some server side error." : "Method is not supported.");
	
	sprintf(headers + strlen(headers),
		"Content-Length: %lu\r\nConnection: close\r\n\r\n", strlen(html_code));
	//build the final response
	sprintf(response, "%s%s", headers, html_code);
	
	//if an error occurred here it is really not good
	if (write_to_socket(socket_fd, response, strlen(response)) < 0)
		perror("write");
}

//send response with a file as a content
int send_file_response(int socket_fd, char *path, char *protocol, char *tb_now, int *code)
{	
	//variables
	struct stat file_info = { 0 };
	struct dirent *file_entity = NULL;
	DIR *folder = NULL;
	char response[KILOBYTE] = { 0 }, file_data[KILOBYTE * 10] = { 0 },
		time_buff_lm[32] = { 0 }, *file_name = NULL;
	int file_fd, bytes_read, i;
		
	//open the file with read operation
	file_fd = open(path, O_RDONLY, S_IRUSR);
	if (file_fd < 0) //check for read permissions for the specific file
	{ 	//no read permissions
		if (errno == EACCES)
			*code = FORBIDDEN;
		else //other errors will be treated as an "internal error"
			*code = INTERNAL_ERROR;
		return FAILURE;
	}
	
	//the file path was validated in parse_path()
	if (lstat(path, &file_info) < 0)
	{
		*code = INTERNAL_ERROR;
		close(file_fd);
		return FAILURE;
	}
	
	//this loop will get the file name
	for (i = strlen(path) - 1; i >= 0; i--)
	{ 
		if (path[i] == '/')
		{
			file_name = &(path[i + 1]);
			break;
		}
	}
	
	//get the type for the html headers
	char *mime_type = get_mime_type(file_name);
	if (!mime_type)
	{
		*code = FORBIDDEN;
		close(file_fd);
		return FAILURE;
	}
	
	path[i] = '\0'; //cut the file name from the path
	folder = opendir(path); //can't fail, path already validated
	if (!folder)
	{ //opendir() could fail only upon system error
		*code = INTERNAL_ERROR;
		close(file_fd);
		return FAILURE;
	}
	path[i] = '/'; //get the file back to the path
	
	//get the dirent structure of the file
	file_entity = readdir(folder);
	while (file_entity)
	{
		if (!strcmp(file_entity->d_name, file_name));
			break;
		file_entity = readdir(folder);
	}
	closedir(folder);
	
	//start to build the headers
	sprintf(response,
		"%s %s\r\nServer: webserver/1.%s\r\nDate: %s\r\nContent-Type: ",
		protocol, code_to_string(OK), protocol[7] == '0'? "0" : "1", tb_now);
	
	//set up the last modified time of the file
	strftime(time_buff_lm, sizeof(time_buff_lm), RFC1123FMT, gmtime(&file_info.st_mtime));
	
	//more to the headers
	sprintf(response + strlen(response),
		"%s\r\nContent-length: %lu\r\nLast-Modified: %s\r\nConnection: close\r\n\r\n",
		mime_type, file_info.st_size, time_buff_lm);

	//write the headers
	if (write_to_socket(socket_fd, response, strlen(response)) < 0)
	{
		perror("write");
		*code = WRITE_ERROR;
		close(file_fd);
		return FAILURE; 
	}
	
	while (1)
	{
		//bytes_read - the size that was read
		bytes_read = read(file_fd, file_data, sizeof(file_data));
		if (bytes_read == 0) //no more to read
		{
			break;
		}
		else if (bytes_read > 0) //there's more to read
		{	//try to write it to the socket
			if (write_to_socket(socket_fd, file_data, bytes_read) < 0)
			{
				perror("write");
				*code = WRITE_ERROR;
				close(file_fd);
				return FAILURE; 
			}
		}
		else //reading from file failed, not related to the socket
		{
			perror("read");
			*code = INTERNAL_ERROR; 
			close(file_fd); 
			return FAILURE;
		}
	}
	write (socket_fd, "\r\n\r\n", 4);
	close(file_fd);
	return SUCCESS;
}

//send a response with the folder information in a table
int send_folder_response(int socket_fd, char *path, char *protocol, char *tb_now, int *code)
{
	//variables
	char headers[KILOBYTE / 2] = { 0 },
	tb_file_lm[32] = { 0 }, tb_folder_lm[32] = { 0 };
	struct stat file_info = { 0 };
	struct dirent *curr_file_entity = NULL;
	DIR *folder;
	
	folder = opendir(path); //can't fail, path already validated
	if (!folder)
	{ //opendir() could fail only upon system error
		*code = INTERNAL_ERROR;
		return FAILURE;
	}
	
	//count the number of files in the folder
	int count = 0;
	curr_file_entity = readdir(folder);
	while (curr_file_entity) 
	{
		if (strcmp (curr_file_entity->d_name, ".")) //ignore the "." 
			count++;
		curr_file_entity = readdir(folder);
	}
	closedir(folder);
	//allocate 512 bits for each file
	char *html_code = (char*)calloc(count * (KILOBYTE / 2), sizeof(char));
	if (!html_code)
	{
		*code = INTERNAL_ERROR;
		return FAILURE;
	}
		
	folder = opendir(path); //can't fail, path already validated
	if (!folder)
	{ //opendir() could fail only upon system error
		*code = INTERNAL_ERROR;
		free(html_code);
		return FAILURE;
	}
	
	if (lstat(path, &file_info) < 0)
	{
		/* the folder path was validated in parse_path(). now when going through 
		the files in the folder the error "forbidden" is the only one possible,
		for that reason we can treat all other errors as an "internal error" */
		*code = INTERNAL_ERROR;
		closedir(folder);
		free(html_code);
		return FAILURE;
	}
	
	//start to build the headers
	sprintf(headers,
		"%s %s\r\nServer: webserver/1.%s\r\nDate: %s\r\nContent-Type: text/html\r\n",
		protocol, code_to_string(OK), protocol[7] == 0? "0" : "1", tb_now);
	
	//set up the last modified time of the folder
	strftime(tb_folder_lm, sizeof(tb_folder_lm), RFC1123FMT, gmtime(&file_info.st_mtime));

	//start to build the html code
	sprintf(html_code,
		"<HTML>\r\n<HEAD><TITLE> Index of %s</TITLE></HEAD>\r\n<BODY>\r\n<H4>Index of %s</H4>\r\n",
		path + 1, path + 1);
	sprintf(html_code + strlen(html_code),
		"<table CELLSPACING=8>\r\n<tr><th>Name</th><th>Last Modified</th><th>Size</th></tr>\r\n");
	
	//go through the files in the folder
	curr_file_entity = readdir(folder);
	while (curr_file_entity) 
	{
		if (!strcmp (curr_file_entity->d_name, ".")) //ignore the "." 
		{
			curr_file_entity = readdir(folder);
			continue;
		}
		//append the current file name to the path
		strcat(path, curr_file_entity->d_name);
		//get the current file information
		if (lstat(path, &file_info) < 0)
		{
			//execute permission is denied for one of the directories in the path
			if (errno == EACCES) 
				*code = FORBIDDEN;
			else //other errors will be treated as syetem errors
				*code = INTERNAL_ERROR;
			return FAILURE;
		}
		
		//set up the last modified time of the current file
		strftime(tb_file_lm, sizeof(tb_file_lm), RFC1123FMT, gmtime(&file_info.st_mtime));
		
		//concatting for each file the following:
		sprintf(html_code + strlen(html_code),
			"<tr><td><A HREF=\"%s\">%s</A></td><td>%s</td><td>",
			curr_file_entity->d_name, curr_file_entity->d_name, tb_file_lm);
		
		//check if the current entity is a file (not a folder)
		if(S_ISREG(file_info.st_mode))
			sprintf(html_code + strlen(html_code), "%lu", file_info.st_size);
		//add closures to the current line
		sprintf(html_code + strlen(html_code), "</td></tr>\r\n");
		
		//this operation will delete the file name for the next one to be appended
		path[strlen(path) - strlen(curr_file_entity->d_name)] = '\0';
		//go to the next file, returns NULL upon end of stream
		curr_file_entity = readdir(folder);
	}	
	
	//more to the html code
	sprintf(html_code + strlen(html_code), 
		"</table>\r\n<HR>\r\n<ADDRESS>webserver/1.%s</ADDRESS>\r\n</HR>\r\n</BODY></HTML>\r\n\r\n",
		protocol[7] == 0? "0" : "1");
	
	//more to the headers
	sprintf(headers + strlen(headers),
		"Content-Length: %lu\r\nLast-Modified: %s\r\nConnection: close\r\n\r\n",
		strlen(html_code), tb_folder_lm);
	
	//allocate the space for the response including the html_code (also allocated)
	char *response = (char*)calloc(strlen(html_code) + (KILOBYTE / 2), sizeof(char));
	if (!response)
	{
		*code = INTERNAL_ERROR;
		free(html_code);
		return FAILURE;
	}
	
	//build the final response
	sprintf(response, "%s%s", headers, html_code);
	
	//write it to the socket
	if (write_to_socket(socket_fd, response, strlen(response)) < 0)
	{
		*code = WRITE_ERROR;
		closedir(folder);
		free(html_code);
		free(response);
		return FAILURE; 
	}
	free(html_code);
	free(response);
	closedir(folder);
	return SUCCESS;
}

//this function will write to the socket
int write_to_socket(int socket_fd, char *msg_to_send, int bytes_to_write)
{
	int bytes_written = 1;
	char *yet_to_send = msg_to_send;
	while (bytes_to_write > 0)
	{
		bytes_written = write(socket_fd, yet_to_send, bytes_to_write);
		if (bytes_written < 0) 
			return FAILURE;
			
		bytes_to_write -= bytes_written;
		yet_to_send += bytes_written;
	}
	return SUCCESS;
}

//this function will parse the args added to the trace from the shell
int parse_args(char *argv[], int *port, int *pool_size, int* max_requests)
{
	if (digits_only(argv[1]) < 0) //check that the port contain only digits
		return FAILURE;
	*port = atoi(argv[1]);
	
	if (*port < 0) //check that the port is not negative
		return FAILURE;
	
	if (digits_only(argv[2]) < 0) //check the number of threads requested
		return FAILURE;
	*pool_size = atoi(argv[2]);
	
	//check that the pool-size reqested is not negative and not exceeds the limit
	if (*pool_size > MAXT_IN_POOL || *pool_size < 0)
		return FAILURE;
	
	//check that the max requests number contain only digits
	if (digits_only(argv[3]) < 0)
		return FAILURE;
	*max_requests = atoi(argv[3]);
		
	//check that the max requests number reqested is positive
	if (*max_requests < 1)
		return FAILURE;
	
	return SUCCESS;
}

//this function will set up the server
int set_up_server(struct sockaddr_in *my_server, int *listen_socket, int port)
{
	//initialize the server
	bzero((char*)my_server, sizeof(struct sockaddr_in));
	my_server->sin_family = AF_INET; //TCP
	my_server->sin_addr.s_addr = INADDR_ANY; //accept any client
	my_server->sin_port = htons(port); //use the given port
	
	//the socket
	*listen_socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (*listen_socket < 0)
	{
		perror("socket");
		return FAILURE;	
	}

	//binding
	if (bind(*listen_socket, (struct sockaddr*)my_server, sizeof(struct sockaddr_in)) < 0)
	{
		perror("bind");
		close(*listen_socket);
		return FAILURE;
	}
	
	//listening
	if (listen(*listen_socket, 5) < 0)
	{
		perror("listen");
		close(*listen_socket);
		return FAILURE;
	}
	
	return SUCCESS;
}

//this function will if a text contains only digits
int digits_only(char* text)
{
	int i;
	for (i = 0; i < strlen(text); i++)
		if (text[i] < 48 || text[i] > 57) //by ASCII 
			return FAILURE;
	return SUCCESS;
}

//the type of file to send in the headers, added a faw of my own
char *get_mime_type(char *name)
{
	char *ext = strrchr(name, '.');
	if (!ext) return NULL;
	if (!strcmp(ext, ".html") || !strcmp(ext, ".htm") || !strcmp(ext, ".txt")) 
		return "text/html";
	if (!strcmp(ext, ".jpg") || !strcmp(ext, ".jpeg")) 
		return "image/jpeg";
	if (!strcmp(ext, ".gif")) 
		return "image/gif";
	if (!strcmp(ext, ".png")) 
		return "image/png";
	if (!strcmp(ext, ".css")) 
		return "text/css";
	if (!strcmp(ext, ".au")) 
		return "audio/basic";
	if (!strcmp(ext, ".wav")) 
		return "audio/wav";
	if (!strcmp(ext, ".avi")) 
		return "video/x-msvideo";
	if (!strcmp(ext, ".mpeg") || !strcmp(ext, ".mpg") == 0) 
		return "video/mpeg";
	if (!strcmp(ext, ".mp3")) 
		return "audio/mpeg";
	return NULL;
}

//will translate the code to a string
char *code_to_string(int code)
{
	if (code == 200) //no error
		return "200 OK";
	else if (code == 302)
		return "302 Found";
	else if (code == 400)
		return "400 Bad Request";
	else if (code == 403)
		return "403 Forbidden";
	else if (code == 404)
		return "404 Not Found";
	else if (code == 500)
		return "500 Internal Server Error";
	else //equal 501
		return "501 Not supported";
}


