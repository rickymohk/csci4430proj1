Group number: 17

Group members:
  Mo Yik Chung 1155048490
  Mung Chun Ting 1155049228
  Wong Cheuk Bun 1155048570

List of files:
  ./client:
    client.c        Main program to be run on client-side
    mtcp_client.c   Implementation of mtcp client-side functions
    mtcp_client.h   Header file of mtcp client-side functions
    mtcp_common.h   A common header file defining some constants
    Makefile        Makefile for making the client-side program
  ./server:
    server.c        Main program to be run on server-side
    mtcp_server.c   Implementation of mtcp server-side functions
    mtcp_server.h   Header file of mtcp server-side functions
    mtcp_common.h   A common header file defining some constants
    Makefile        Makefile for making the server-side program

Method of compilation:
    cd ./client
    make
    cd ../server
    make

Method of execution on server-side:
  cd ./server
  ./server [server address] [output filename]

Method of exection on client-side:
  cd ./client
  ./client [server address] [input filename]
