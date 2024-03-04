\documentclass[11pt]{article}
\usepackage{hyperref}
\usepackage{geometry}
\geometry{a4paper, margin=1in}

\title{Asynchronous Web Server in C}
\author{}
\date{}

\begin{document}

\maketitle

\section*{Overview}

This project showcases a high-performance asynchronous web server implemented in C, utilizing advanced Linux features such as \texttt{libaio}, \texttt{epoll}, and \texttt{eventfd} for efficient I/O operations. Designed with a focus on non-blocking, event-driven architecture, this server is capable of handling a high volume of concurrent connections, making it a prime example of low-level system programming in a modern Linux environment.

\section*{Features}

\begin{itemize}
    \item \textbf{Asynchronous File I/O:} Leverages \texttt{libaio} for non-blocking file reads, allowing the server to serve dynamic content efficiently.
    \item \textbf{Event-Driven Networking:} Utilizes \texttt{epoll} for scalable event notification, enabling the server to manage thousands of simultaneous connections.
    \item \textbf{Efficient Data Transfer:} Employs \texttt{sendfile} for zero-copy transfers of static content directly from disk to network, minimizing CPU usage.
    \item \textbf{Robust HTTP Parsing:} Integrates a lightweight yet comprehensive HTTP parser to handle requests accurately.
    \item \textbf{Time and Date Handling:} Implements RFC-compliant date formatting for HTTP headers, enhancing compatibility with web standards.
\end{itemize}

\section*{Getting Started}

\subsection*{Prerequisites}

\begin{itemize}
    \item Linux environment
    \item GCC or Clang compiler
    \item libaio library
    \item Basic knowledge of network programming in C
\end{itemize}

\subsection*{Compilation}

To compile the server, use the following command:

\begin{verbatim}
gcc -o async_web_server server.c -laio -lpthread
\end{verbatim}

\subsection*{Running the Server}

To run the server, execute:

\begin{verbatim}
./async_web_server
\end{verbatim}

The server listens on port 8080 by default. Adjust the \texttt{AWS\_LISTEN\_PORT} macro in \texttt{aws.h} to change the default port.

\section*{Usage}

Once running, the server can serve static and dynamic content from specified directories. Static content is served from a directory named \texttt{static} and dynamic content from \texttt{dynamic}.

\section*{Testing}

You can test the server's functionality using any HTTP client, such as curl:

\begin{verbatim}
curl http://localhost:8080/static/index.html
\end{verbatim}

\section*{Design and Implementation}

\subsection*{Architecture Overview}

\begin{itemize}
    \item \textbf{Main Loop:} The server uses an epoll-based loop to efficiently multiplex incoming connections and I/O events.
    \item \textbf{Asynchronous I/O:} Dynamic content requests are handled asynchronously using libaio, with eventfd for notification.
    \item \textbf{Connection Management:} Each client connection is represented by a struct connection, facilitating organized management of state and data.
\end{itemize}

\subsection*{Detailed Workflow}

\begin{enumerate}
    \item Initialization: Set up epoll instance and listening socket.
    \item Accepting Connections: New connections are non-blocking and added to epoll.
    \item Request Handling: Asynchronous reads for dynamic content and efficient \texttt{sendfile} for static content.
    \item Response Preparation: HTTP headers are crafted, with proper date and last-modified headers.
    \item Cleanup: Resources are freed and connections are closed gracefully.
\end{enumerate}

\section*{Contributing}

Contributions are welcome! For major changes, please open an issue first to discuss what you would like to change. Please ensure to update tests as appropriate.

\section*{License}

This project is licensed under the BSD-3-Clause License - see the LICENSE file for details.

\end{document}
