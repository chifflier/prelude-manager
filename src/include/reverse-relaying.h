int reverse_relay_tell_receiver_alive(prelude_connection_t *cnx);

int reverse_relay_tell_dead(prelude_connection_t *cnx);

int reverse_relay_add_receiver(prelude_connection_t *cnx);

prelude_connection_t *reverse_relay_search_receiver(const char *addr);

void reverse_relay_send_msg(idmef_message_t *idmef);

int reverse_relay_create_initiator(const char *arg);

int reverse_relay_init_initiator(void);
