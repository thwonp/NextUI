#if defined(VERTEX)
attribute vec4 VertexCoord;
attribute vec2 TexCoord;
uniform mat4 MVPMatrix;
varying vec2 vTexCoord;

void main() {
    // Flip Y-axis in the vertex shader
    vTexCoord = vec2(TexCoord.x, TexCoord.y);  // Flip Y
    gl_Position = MVPMatrix * VertexCoord;
}
#endif

#if defined(FRAGMENT)
precision mediump float;
uniform sampler2D Texture;
varying vec2 vTexCoord;

void main() {
    gl_FragColor = texture2D(Texture, vTexCoord);
}
#endif
