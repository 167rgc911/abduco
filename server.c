#define FD_SET_MAX(fd, set, maxfd) do { \
		FD_SET(fd, set);        \
		if (fd > maxfd)         \
			maxfd = fd;     \
	} while (0)

static Client *client_malloc(int socket) {
	Client *c = calloc(1, sizeof(Client));
	if (!c)
		return NULL;
	c->socket = socket;
	return c;
}

static void client_free(Client *c) {
	if (c && c->socket > 0)
		close(c->socket);
	free(c);
}

static int server_set_socket_non_blocking(int sock) {
	int flags;
	if ((flags = fcntl(sock, F_GETFL, 0)) == -1)
		flags = 0;
    	return fcntl(sock, F_SETFL, flags | O_NONBLOCK);
}

static Client *server_accept_client(time_t now) {
	int newfd = accept(server.socket, NULL, NULL);
	if (newfd == -1)
		return NULL;
	Client *c = client_malloc(newfd);
	if (!c)
		return NULL;
	server_set_socket_non_blocking(newfd);
	c->socket = newfd;
	c->state = STATE_CONNECTED;
	c->last_activity = now;
	c->next = server.clients;
	server.clients = c;
	server.client_count++;
	return c;
}

static bool server_read_pty(ServerPacket *pkt) {
	ssize_t len = read(server.pty, pkt->buf, sizeof(pkt->buf));
	if (len != -1)
		pkt->len = len;
	else if (errno != EAGAIN && errno != EINTR)
		server.running = false;
	print_server_packet("server-read-pty:", pkt);
	return len > 0;
}

static bool server_write_pty(ClientPacketState *pkt) {
	int count = pkt->pkt.len - pkt->off; 
	ssize_t len = write(server.pty, pkt->pkt.u.msg + pkt->off, count);
	if (len == -1) {
		if (errno != EAGAIN && errno != EINTR)
			server.running = false;
	} else { 
		pkt->off += len;
	}
	print_client_packet_state("server-write-pty:", pkt);
	return len == count;
}

static void server_place_packet(Client *c, ServerPacket *pkt) {
	c->output.pkt = pkt;
	c->output.off = 0;
}

static bool server_recv_packet(Client *c) {
	ClientPacketState *pkt = &c->input;
	if (is_client_packet_complete(pkt))
		return true;
	size_t count = sizeof(ClientPacket) - pkt->off;
	ssize_t len = recv(c->socket, ((char *)&pkt->pkt) + pkt->off, count, 0);
	switch (len) {
	case -1:
		if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
	case 0:
			c->state = STATE_DISCONNECTED;
		}
		break;
	default:
		pkt->off += len;
		break;
	}
	print_client_packet_state("server-recv:", pkt);
	return len == count;
}

static bool server_send_packet(Client *c) {
	ServerPacketState *pkt = &c->output;
	if (is_server_packet_complete(pkt))
		return true;
	size_t count = pkt->pkt->len - pkt->off;
	ssize_t len = send(c->socket, pkt->pkt->buf + pkt->off, count, 0);
	switch (len) {
	case -1:
		if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
	case 0:
			c->state = STATE_DISCONNECTED;
		}
		break;
	default:
		pkt->off += len;
		break;
	}
	print_server_packet_state("server-send:", pkt);
	return len == count;
}

static void server_pty_died_handler(int sig) {
	int errsv = errno;
	pid_t pid;

	while ((pid = waitpid(-1, &server.exit_status, WNOHANG)) != 0) {
		if (pid == -1)
			break;
		server.exit_status = WEXITSTATUS(server.exit_status);
	}

	debug("server pty died: %d\n", server.exit_status);
	errno = errsv;
}

static void server_sigterm_handler(int sig) {
	exit(EXIT_FAILURE); /* invoke atexit handler */
}

static void server_atexit_handler() {
	unlink(sockaddr.sun_path);
}

static bool server_queue_empty() {
	return server.queue_count == 0;
}

static bool server_queue_packet(ClientPacket *pkt) {
	if (server.queue_count >= countof(server.queue))
		return false;
	server.queue[server.queue_insert] = *pkt;
	server.queue_insert++;
	server.queue_insert %= countof(server.queue);
	server.queue_count++;
	return true;
}

static ClientPacket *server_peek_packet() {
	return &server.queue[server.queue_remove];
}

static void server_dequeue_packet() {
	server.queue_remove++;
	server.queue_remove %= countof(server.queue);
	server.queue_count--;
}

static void server_mainloop() {
	atexit(server_atexit_handler);
	fd_set new_readfds, new_writefds;
	FD_ZERO(&new_readfds);
	FD_ZERO(&new_writefds);
	FD_SET(server.socket, &new_readfds);
	int new_fdmax = server.socket;

	for (;;) {
		int fdmax = new_fdmax;
		fd_set readfds = new_readfds;
		fd_set writefds = new_writefds;

		if (select(fdmax+1, &readfds, &writefds, NULL, NULL) == -1) {
			if (errno == EINTR)
				continue;
			die("server-mainloop");
		}

		FD_ZERO(&new_readfds);
		FD_SET(server.socket, &new_readfds);
		FD_ZERO(&new_writefds);
		new_fdmax = server.socket;

		time_t now = time(NULL);
		time_t timeout = now - CLIENT_TIMEOUT;
		bool pty_data = false, clients_ready = true;

		if (FD_ISSET(server.socket, &readfds))
			server_accept_client(now);

		if (FD_ISSET(server.pty, &readfds)) {
			pty_data = server_read_pty(&server.pty_output);
			clients_ready = !pty_data;
		}

		for (Client **prev_next = &server.clients, *c = server.clients; c;) {
			if (c->state == STATE_DISCONNECTED) {
				Client *t = c->next;
				client_free(c);
				*prev_next = c = t;
				server.client_count--;
				continue;
			}

			if (FD_ISSET(c->socket, &readfds))
				server_recv_packet(c);
			if (is_client_packet_complete(&c->input)) {
				bool packet_handled = true;
				switch (c->input.pkt.type) {
				case MSG_CONTENT:
					packet_handled = server_queue_packet(&c->input.pkt);
					break;
				case MSG_ATTACH:
				case MSG_RESIZE:
					c->state = STATE_ATTACHED;
					ioctl(server.pty, TIOCSWINSZ, &c->input.pkt.u.ws);
				case MSG_REDRAW:
					kill(-server.pid, SIGWINCH);
					break;
				case MSG_DETACH:
					c->state = STATE_DETACHED;
					break;
				default: /* ignore package */
					break;
				}

				if (packet_handled) {
					c->input.off = 0;
					FD_SET_MAX(c->socket, &new_readfds, new_fdmax);
				}
			} else {
				FD_SET_MAX(c->socket, &new_readfds, new_fdmax);
			}

			if (pty_data) {
				server_place_packet(c, &server.pty_output);
				c->last_activity = now;
			}
	
			if (FD_ISSET(c->socket, &writefds)) {
				server_send_packet(c);
				c->last_activity = now;
			}

			if (!is_server_packet_complete(&c->output)) {
				if (c->last_activity < timeout) {
					c->state = STATE_DISCONNECTED;
				} else if (is_server_packet_nonempty(&c->output)) {
					clients_ready = false;
					FD_SET_MAX(c->socket, &new_writefds, new_fdmax);
				}
			}

			if (c->state != STATE_ATTACHED)
				clients_ready = false;
			prev_next = &c->next;
			c = c->next;
		}

		if (clients_ready && server.clients) {
			if (server.running)
				FD_SET_MAX(server.pty, &new_readfds, new_fdmax);
			else
				break;
		}

		if (FD_ISSET(server.pty, &writefds)) {
			while (!server_queue_empty()) {
				if (!server.pty_input.off)
					server.pty_input.pkt = *server_peek_packet();
				if (!server_write_pty(&server.pty_input))
					break;
				server_dequeue_packet();
				server.pty_input.off = 0;
			}
		}

		if (!server_queue_empty())
			FD_SET_MAX(server.pty, &new_writefds, new_fdmax);
	}

	exit(EXIT_SUCCESS);
}
