// web/tests/baton/baton_test.cpp links the product's disc library: there is no disc on the test
// page, so it must not fetch a manifest at startup.
Module['mkwDiscPrefetchManifest'] = false;
