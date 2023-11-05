import os
import select
import time


class PipeFail(Exception):
    pass

class PipeManager:
    """Class for managing pipes of the child process"""


    timeout_default: int = 2

    def __init__(self, stdin_write: int, stdout_read: int, stderr_read: int):
        """Constructor for PipeManager class
        
        Args:
            stdin_write (int): file descriptor for stdin write
            stdout_read (int): file descriptor for stdout read
            stderr_read (int): file descriptor for stderr read
        """
        self.stdin_write: int = stdin_write
        self.stdout_read: int = stdout_read
        self.stderr_read: int = stderr_read
    
    
    def recv(self, numb: int=None, timeout: int=timeout_default) -> bytes:
        """Receives at most numb bytes from the child process stdout.
        
        Args:
            numb (int, optional): number of bytes to receive. Defaults to None.
            timeout (int, optional): timeout in seconds. Defaults to timeout_default.

        Returns:
            bytes: received bytes from the child process stdout

        Raises:
            ValueError: numb is negative
            PipeFail: no stdout pipe of the child process
        """

        if not self.stdout_read:
            raise PipeFail("No stdout pipe of the child process")
        
        # Buffer for the received data
        data_buffer = b''

        if numb:
            # Checking the numb
            if numb < 0:
                raise ValueError("The number of bytes to receive must be positive")

            # Setting the alarm
            end_time = time.time() + timeout
            while numb > 0:
                if end_time is not None and time.time() > end_time:
                    # Timeout reached
                    break

                # Adjust the timeout for select to the remaining time
                remaining_time = None if end_time is None else max(0, end_time - time.time())
                ready, _, _ = select.select([self.stdout_read], [], [], remaining_time)

                if not ready:
                    # No data ready within the remaining timeout
                    break

                data = os.read(self.stdout_read, numb)
                if not data:
                    # No more data available
                    break

                numb -= len(data)
                data_buffer += data
        else:
            ready, _, _ = select.select([self.stdout_read], [], [], timeout)
            
            if ready:
                # Read all available bytes up to 4096
                data = os.read(self.stdout_read, 4096)
                data_buffer += data
                    
        return data_buffer