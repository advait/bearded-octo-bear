CS118 - Project 1
=================
Advait Shinde - 403670269

Design Decisions:

Since I really wanted to learn event-based programming, I implemented this 
project with an event-loop. It turned out great! First, I set up all sockets
to be non-blocking. Then I create a server socket and bind it to port 12345.
Then, I start my event loop. The loop is pretty straightforward:

  - First determine all the sockets we need to poll on (this will always be
    an array of sockets that includes the server socket).
  - Poll on these sockets (man 2 poll)
  - If we get an event on the server socket, initiate a connection (accept)
    and create a new ProxyState object that will keep track of the state of
    this ProxyConnection. The states go as follows:
      1. Wait to read request from client
      2. Wait to write request to upstream authoritative server
      3. Wait to read response from upstream server
      4. Wait to write response back to client
  - I also maintain a map that maps file descriptors to their corresponding
    ProxyState objects. This way, when an event happens on a given fd, I know
    which ProxyState object will contain information about that fd and what
    state this Proxy transaction is in.
  - If we get an event on a non-server socket, use the map to look up which
    ProxyState object the fd corresponds to and call advanceState() on that
    object. This method is the meat of the project and handles all the
    socket creation, recving, and sending.

Unfortunately, I'm running into some errors with the test script (which
results in an HTTP 400 Bad Request). However, everything else seems to be
implemented correctly.
