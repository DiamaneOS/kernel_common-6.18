USB gadget sessions and port policy
===================================

USB gadget function rebinding does not end a physical cable session.
The host enumeration gate ``deny_new_usb2`` must not treat every gadget
pullup as a new physical connection. Host enumeration protection remains
in the USB hub core; gadget session policy belongs to the port controller.

Device integration must independently refuse new host and peripheral
sessions while disabled, revoke retention on detach or policy disable,
and retain only an existing session in keep mode. Repeated keep writes
must not reauthorize a revoked session. Unknown states must fail closed.

A shared kernel must not be used on a device lacking an equivalent
controller policy implementation.
