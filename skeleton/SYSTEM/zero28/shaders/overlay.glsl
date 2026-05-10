#if defined(VERTEX)
attribute vec4 VertexCoord;
attribute vec2 TexCoord;
uniform mat4 MVPMatrix;
varying vec2 vTexCoord;

void main() {
    vTexCoord = TexCoord;
    gl_Position = MVPMatrix * VertexCoord;
}
#endif

#if defined(FRAGMENT)
precision mediump float;
uniform sampler2D Texture;
varying vec2 vTexCoord;

void main() {
    gl_FragColor = texture2D(Texture, vec2(vTexCoord.x, 1.0 - vTexCoord.y));
}
#endif
