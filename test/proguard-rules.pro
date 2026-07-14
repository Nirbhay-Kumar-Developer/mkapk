-keepclassmembers class * extends android.app.Activity {
    public void *(android.view.View);
}

-keep class * extends android.app.Activity
-keep class * extends android.app.Application
-keep class * extends android.app.Fragment

# Fix: Restored the missing class header targeting custom View classes
-keep class * extends android.view.View {
    public <init>(android.content.Context);
    public <init>(android.content.Context, android.util.AttributeSet);
    public <init>(android.content.Context, android.util.AttributeSet, int);
}

# Keep classes that are Serializable
-keep class * implements java.io.Serializable {
    static final long serialVersionUID;
    private static final java.io.ObjectStreamField[] serialPersistentFields;
    private void writeObject(java.io.ObjectOutputStream);
    private void readObject(java.io.ObjectInputStream);
    boolean *(***);
    java.lang.Object *(***);
}