// After creating package.json, install modules:
//   $ npm install
//
// Launch server with:
//   $ node server.js
var PORT_NUMBER = 8088;

// TODO: handle wav file uploads with express.js maybe?
var http = require('http');
var fs   = require('fs');
var path = require('path');
var mime = require('mime');

/* 
 * Create the static web server
 */
var server = http.createServer(function(request, response) {
	var filePath = false;
	
	if (request.url == '/') {
		filePath = 'public/index.html';
	} else {
		filePath = 'public' + request.url;
	}
	
	var absPath = './' + filePath;
	serveStatic(response, absPath);
});

server.listen(PORT_NUMBER, function() {
	console.log("Server listeneing on port " + PORT_NUMBER);
	console.log("Connect via 192.168.7.2:" + PORT_NUMBER);
});

function serveStatic(response, absPath) {
	fs.exists(absPath, function(exists) {
		if (exists) {
			fs.readFile(absPath, function(err, data) {
				if (err) {
					send404(response);
				} else {
					sendFile(response, absPath, data);
				}
			});
		} else {
			send404(response);
		}
	});
}

function send404(response) {
	response.writeHead(404, {'Content-Type': 'text/plain'});
	response.write('Error 404: resource not found.');
	response.end();
}

function sendFile(response, filePath, fileContents) {
	response.writeHead(
			200,
			{"content-type": mime.lookup(path.basename(filePath))}
		);
	response.end(fileContents);
}


/*
 * Create the Userver to listen for the websocket
 */
var backend = require('./lib/admin_console_server');
backend.listen(server);

