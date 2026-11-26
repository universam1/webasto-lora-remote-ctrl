'use strict';
// Node 22 treats 'node --test pwa/test/' as a module path; this entry makes that command work.
require('./parser.test.js');
require('./history.test.js');
require('./charts.test.js');
require('./util.test.js');
