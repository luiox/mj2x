package mj2x;

import java.lang.annotation.ElementType;
import java.lang.annotation.Retention;
import java.lang.annotation.RetentionPolicy;
import java.lang.annotation.Target;

/**
 * Marks a Java method as eligible for mj2x native translation.
 *
 * Compile targets can select this annotation with:
 *
 * <pre>
 * --native-annotation Lmj2x/Native;
 * </pre>
 */
@Retention(RetentionPolicy.CLASS)
@Target(ElementType.METHOD)
public @interface Native {
}
